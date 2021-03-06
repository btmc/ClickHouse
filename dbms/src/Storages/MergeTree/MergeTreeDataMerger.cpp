#include <DB/Storages/MergeTree/MergeTreeDataMerger.h>
#include <DB/Storages/MergeTree/MergeTreeBlockInputStream.h>
#include <DB/Storages/MergeTree/MergedBlockOutputStream.h>
#include <DB/Storages/MergeTree/DiskSpaceMonitor.h>
#include <DB/Storages/MergeTree/MergeTreeSharder.h>
#include <DB/Storages/MergeTree/ReshardingJob.h>
#include <DB/Storages/MergeTree/SimpleMergeSelector.h>
#include <DB/Storages/MergeTree/AllMergeSelector.h>
#include <DB/Storages/MergeTree/MergeList.h>
#include <DB/DataStreams/ExpressionBlockInputStream.h>
#include <DB/DataStreams/MergingSortedBlockInputStream.h>
#include <DB/DataStreams/CollapsingSortedBlockInputStream.h>
#include <DB/DataStreams/SummingSortedBlockInputStream.h>
#include <DB/DataStreams/ReplacingSortedBlockInputStream.h>
#include <DB/DataStreams/GraphiteRollupSortedBlockInputStream.h>
#include <DB/DataStreams/AggregatingSortedBlockInputStream.h>
#include <DB/DataStreams/MaterializingBlockInputStream.h>
#include <DB/DataStreams/ConcatBlockInputStream.h>
#include <DB/DataStreams/ColumnGathererStream.h>
#include <DB/Storages/MergeTree/BackgroundProcessingPool.h>
#include <DB/Common/Increment.h>
#include <DB/Common/interpolate.h>

#include <cmath>
#include <numeric>


namespace ProfileEvents
{
	extern const Event MergedRows;
	extern const Event MergedUncompressedBytes;
}

namespace CurrentMetrics
{
	extern const Metric BackgroundPoolTask;
}

namespace DB
{

namespace ErrorCodes
{
	extern const int ABORTED;
}


using MergeAlgorithm = MergeTreeDataMerger::MergeAlgorithm;


namespace
{

std::string createMergedPartName(const MergeTreeData::DataPartsVector & parts)
{
	DayNum_t left_date = DayNum_t(std::numeric_limits<UInt16>::max());
	DayNum_t right_date = DayNum_t(std::numeric_limits<UInt16>::min());
	UInt32 level = 0;

	for (const MergeTreeData::DataPartPtr & part : parts)
	{
		level = std::max(level, part->level);
		left_date = std::min(left_date, part->left_date);
		right_date = std::max(right_date, part->right_date);
	}

	return ActiveDataPartSet::getPartName(left_date, right_date, parts.front()->left, parts.back()->right, level + 1);
}

}

/// Не будем соглашаться мерджить куски, если места на диске менее чем во столько раз больше суммарного размера кусков.
static const double DISK_USAGE_COEFFICIENT_TO_SELECT = 1.6;

/// Объединяя куски, зарезервируем столько места на диске. Лучше сделать немного меньше, чем DISK_USAGE_COEFFICIENT_TO_SELECT,
/// потому что между выбором кусков и резервированием места места может стать немного меньше.
static const double DISK_USAGE_COEFFICIENT_TO_RESERVE = 1.4;

MergeTreeDataMerger::MergeTreeDataMerger(MergeTreeData & data_, const BackgroundProcessingPool & pool_)
	: data(data_), pool(pool_), log(&Logger::get(data.getLogName() + " (Merger)"))
{
}

void MergeTreeDataMerger::setCancellationHook(CancellationHook cancellation_hook_)
{
	cancellation_hook = cancellation_hook_;
}


size_t MergeTreeDataMerger::getMaxPartsSizeForMerge()
{
	size_t total_threads_in_pool = pool.getNumberOfThreads();
	size_t busy_threads_in_pool = CurrentMetrics::values[CurrentMetrics::BackgroundPoolTask].load(std::memory_order_relaxed);

	return getMaxPartsSizeForMerge(total_threads_in_pool, busy_threads_in_pool == 0 ? 0 : busy_threads_in_pool - 1); /// 1 is current thread
}


size_t MergeTreeDataMerger::getMaxPartsSizeForMerge(size_t pool_size, size_t pool_used)
{
	if (pool_used > pool_size)
		throw Exception("Logical error: invalid arguments passed to getMaxPartsSizeForMerge: pool_used > pool_size", ErrorCodes::LOGICAL_ERROR);

	size_t free_entries = pool_size - pool_used;

	size_t max_size = 0;
	if (free_entries >= data.settings.number_of_free_entries_in_pool_to_lower_max_size_of_merge)
		max_size = data.settings.max_bytes_to_merge_at_max_space_in_pool;
	else
		max_size = interpolateExponential(
			data.settings.max_bytes_to_merge_at_min_space_in_pool,
			data.settings.max_bytes_to_merge_at_max_space_in_pool,
			static_cast<double>(free_entries) / data.settings.number_of_free_entries_in_pool_to_lower_max_size_of_merge);

	return std::min(max_size, static_cast<size_t>(DiskSpaceMonitor::getUnreservedFreeSpace(data.full_path) / DISK_USAGE_COEFFICIENT_TO_SELECT));
}


bool MergeTreeDataMerger::selectPartsToMerge(
	MergeTreeData::DataPartsVector & parts,
	String & merged_name,
	bool aggressive,
	size_t max_total_size_to_merge,
	const AllowedMergingPredicate & can_merge_callback)
{
	parts.clear();

	MergeTreeData::DataPartsVector data_parts = data.getDataPartsVector();

	if (data_parts.empty())
		return false;

	time_t current_time = time(nullptr);

	IMergeSelector::Partitions partitions;

	DayNum_t prev_month = DayNum_t(-1);
	const MergeTreeData::DataPartPtr * prev_part = nullptr;
	for (const MergeTreeData::DataPartPtr & part : data_parts)
	{
		DayNum_t month = part->month;
		if (month != prev_month || (prev_part && !can_merge_callback(*prev_part, part)))
		{
			if (partitions.empty() || !partitions.back().empty())
				partitions.emplace_back();
			prev_month = month;
		}

		IMergeSelector::Part part_info;
		part_info.size = part->size_in_bytes;
		part_info.age = current_time - part->modification_time;
		part_info.level = part->level;
		part_info.data = &part;

		partitions.back().emplace_back(part_info);

		/// Check for consistenty of data parts. If assertion is failed, it requires immediate investigation.
		if (prev_part && part->month == (*prev_part)->month && part->left < (*prev_part)->right)
		{
			LOG_ERROR(log, "Part " << part->name << " intersects previous part " << (*prev_part)->name);
		}

		prev_part = &part;
	}

	std::unique_ptr<IMergeSelector> merge_selector;

	SimpleMergeSelector::Settings merge_settings;
	if (aggressive)
		merge_settings.base = 1;

	/// NOTE Could allow selection of different merge strategy.
	merge_selector = std::make_unique<SimpleMergeSelector>(merge_settings);

	IMergeSelector::PartsInPartition parts_to_merge = merge_selector->select(
		partitions,
		max_total_size_to_merge);

	if (parts_to_merge.empty())
		return false;

	if (parts_to_merge.size() == 1)
		throw Exception("Logical error: merge selector returned only one part to merge", ErrorCodes::LOGICAL_ERROR);

	parts.reserve(parts_to_merge.size());

	DayNum_t left_date = DayNum_t(std::numeric_limits<UInt16>::max());
	DayNum_t right_date = DayNum_t(std::numeric_limits<UInt16>::min());
	UInt32 level = 0;

	for (IMergeSelector::Part & part_info : parts_to_merge)
	{
		const MergeTreeData::DataPartPtr & part = *static_cast<const MergeTreeData::DataPartPtr *>(part_info.data);

		parts.push_back(part);

		level = std::max(level, part->level);
		left_date = std::min(left_date, part->left_date);
		right_date = std::max(right_date, part->right_date);
	}

	merged_name = ActiveDataPartSet::getPartName(
		left_date, right_date, parts.front()->left, parts.back()->right, level + 1);

	LOG_DEBUG(log, "Selected " << parts.size() << " parts from " << parts.front()->name << " to " << parts.back()->name);
	return true;
}


bool MergeTreeDataMerger::selectAllPartsToMergeWithinPartition(
	MergeTreeData::DataPartsVector & what,
	String & merged_name,
	size_t available_disk_space,
	const AllowedMergingPredicate & can_merge,
	DayNum_t partition,
	bool final)
{
	MergeTreeData::DataPartsVector parts = selectAllPartsFromPartition(partition);

	if (parts.empty())
		return false;

	if (!final && parts.size() == 1)
		return false;

	MergeTreeData::DataPartsVector::const_iterator it = parts.begin();
	MergeTreeData::DataPartsVector::const_iterator prev_it = it;

	size_t sum_bytes = 0;
	DayNum_t left_date = DayNum_t(std::numeric_limits<UInt16>::max());
	DayNum_t right_date = DayNum_t(std::numeric_limits<UInt16>::min());
	UInt32 level = 0;

	while (it != parts.end())
	{
		if ((it != parts.begin() || parts.size() == 1)	/// Для случая одного куска, проверяем, что его можно мерджить "самого с собой".
			&& !can_merge(*prev_it, *it))
			return false;

		level = std::max(level, (*it)->level);
		left_date = std::min(left_date, (*it)->left_date);
		right_date = std::max(right_date, (*it)->right_date);

		sum_bytes += (*it)->size_in_bytes;

		prev_it = it;
		++it;
	}

	/// Достаточно места на диске, чтобы покрыть новый мердж с запасом.
	if (available_disk_space <= sum_bytes * DISK_USAGE_COEFFICIENT_TO_SELECT)
	{
		time_t now = time(0);
		if (now - disk_space_warning_time > 3600)
		{
			disk_space_warning_time = now;
			LOG_WARNING(log, "Won't merge parts from " << parts.front()->name << " to " << (*prev_it)->name
				<< " because not enough free space: "
				<< formatReadableSizeWithBinarySuffix(available_disk_space) << " free and unreserved "
				<< "(" << formatReadableSizeWithBinarySuffix(DiskSpaceMonitor::getReservedSpace()) << " reserved in "
				<< DiskSpaceMonitor::getReservationCount() << " chunks), "
				<< formatReadableSizeWithBinarySuffix(sum_bytes)
				<< " required now (+" << static_cast<int>((DISK_USAGE_COEFFICIENT_TO_SELECT - 1.0) * 100)
				<< "% on overhead); suppressing similar warnings for the next hour");
		}
		return false;
	}

	what = parts;
	merged_name = ActiveDataPartSet::getPartName(
		left_date, right_date, parts.front()->left, parts.back()->right, level + 1);

	LOG_DEBUG(log, "Selected " << parts.size() << " parts from " << parts.front()->name << " to " << parts.back()->name);
	return true;
}


MergeTreeData::DataPartsVector MergeTreeDataMerger::selectAllPartsFromPartition(DayNum_t partition)
{
	MergeTreeData::DataPartsVector parts_from_partition;

	MergeTreeData::DataParts data_parts = data.getDataParts();

	for (MergeTreeData::DataParts::iterator it = data_parts.cbegin(); it != data_parts.cend(); ++it)
	{
		const MergeTreeData::DataPartPtr & current_part = *it;
		DayNum_t month = current_part->month;
		if (month != partition)
			continue;

		parts_from_partition.push_back(*it);
	}

	return parts_from_partition;
}


/// PK columns are sorted and merged, ordinary columns are gathered using info from merge step
static void extractMergingAndGatheringColumns(const NamesAndTypesList & all_columns, ExpressionActionsPtr primary_key_expressions,
	const MergeTreeData::MergingParams & merging_params,
	NamesAndTypesList & gathering_columns, Names & gathering_column_names,
	NamesAndTypesList & merging_columns, Names & merging_column_names
)
{
	Names key_columns_dup = primary_key_expressions->getRequiredColumns();
	std::set<String> key_columns(key_columns_dup.cbegin(), key_columns_dup.cend());

	/// Force sign column for Collapsing mode
	if (merging_params.mode == MergeTreeData::MergingParams::Collapsing)
		key_columns.emplace(merging_params.sign_column);

	/// TODO: also force "summing" and "aggregating" columns to make Horizontal merge only for such columns

	for (auto & column : all_columns)
	{
		auto it = std::find(key_columns.cbegin(), key_columns.cend(), column.name);

		if (key_columns.end() == it)
		{
			gathering_columns.emplace_back(column);
			gathering_column_names.emplace_back(column.name);
		}
		else
		{
			merging_columns.emplace_back(column);
			merging_column_names.emplace_back(column.name);
		}
	}
}

/* Allow to compute more accurate progress statistics */
class ColumnSizeEstimator
{
	MergeTreeData::DataPart::ColumnToSize map;
public:

	/// Stores approximate size of columns in bytes
	/// Exact values are not required since it used for relative values estimation (progress).
	size_t sum_total = 0;
	size_t sum_index_columns = 0;
	size_t sum_ordinary_columns = 0;

	ColumnSizeEstimator(const MergeTreeData::DataPart::ColumnToSize & map_, const Names & key_columns, const Names & ordinary_columns)
		: map(map_)
	{
		for (const auto & name : key_columns)
			if (!map.count(name)) map[name] = 0;
		for (const auto & name : ordinary_columns)
			if (!map.count(name)) map[name] = 0;

		for (const auto & name : key_columns)
			sum_index_columns += map.at(name);

		for (const auto & name : ordinary_columns)
			sum_ordinary_columns += map.at(name);

		sum_total = std::max(1UL, sum_index_columns + sum_ordinary_columns);
	}

	/// Approximate size of num_rows column elements if column contains num_total_rows elements
	Float64 columnSize(const String & column, size_t num_rows, size_t num_total_rows) const
	{
		return static_cast<Float64>(map.at(column)) / num_total_rows * num_rows;
	}

	/// Relative size of num_rows column elements (in comparison with overall size of all columns) if column contains num_total_rows elements
	Float64 columnProgress(const String & column, size_t num_rows, size_t num_total_rows) const
	{
		return columnSize(column, num_rows, num_total_rows) / sum_total;
	}

	/// Like columnSize, but takes into account only PK columns
	Float64 keyColumnsSize(size_t num_rows, size_t num_total_rows) const
	{
		return static_cast<Float64>(sum_index_columns) / num_total_rows * num_rows;
	}

	/// Like columnProgress, but takes into account only PK columns
	Float64 keyColumnsProgress(size_t num_rows, size_t num_total_rows) const
	{
		return keyColumnsSize(num_rows, num_total_rows) / sum_total;
	}
};


class MergeProgressCallback : public ProgressCallback
{
public:
	MergeProgressCallback(MergeList::Entry & merge_entry_) : merge_entry(merge_entry_) {}

	MergeProgressCallback(MergeList::Entry & merge_entry_, MergeTreeDataMerger::MergeAlgorithm merge_alg_, size_t num_total_rows,
						  const ColumnSizeEstimator & column_sizes)
	: merge_entry(merge_entry_), merge_alg(merge_alg_)
	{
		if (merge_alg == MergeAlgorithm::Horizontal)
			average_elem_progress = 1.0 / num_total_rows;
		else
			average_elem_progress = column_sizes.keyColumnsProgress(1, num_total_rows);
	}

	MergeList::Entry & merge_entry;
	const MergeAlgorithm merge_alg{MergeAlgorithm::Vertical};
	Float64 average_elem_progress;

	void operator() (const Progress & value)
	{
		ProfileEvents::increment(ProfileEvents::MergedUncompressedBytes, value.bytes);
		merge_entry->bytes_read_uncompressed += value.bytes;
		merge_entry->rows_with_key_columns_read += value.rows;

		if (merge_alg == MergeAlgorithm::Horizontal)
		{
			ProfileEvents::increment(ProfileEvents::MergedRows, value.rows);
			merge_entry->rows_read += value.rows;
			merge_entry->progress = average_elem_progress * merge_entry->rows_read;
		}
		else
		{
			merge_entry->progress = average_elem_progress * merge_entry->rows_with_key_columns_read;
		}
	};
};

class MergeProgressCallbackVerticalStep : public MergeProgressCallback
{
public:

	MergeProgressCallbackVerticalStep(MergeList::Entry & merge_entry_, size_t num_total_rows_exact,
								  const ColumnSizeEstimator & column_sizes, const String & column_name)
	: MergeProgressCallback(merge_entry_), initial_progress(merge_entry->progress)
	{
		average_elem_progress = column_sizes.columnProgress(column_name, 1, num_total_rows_exact);
	}

	Float64 initial_progress;
	/// NOTE: not thread safe (to be copyable). It is OK in current single thread use case
	size_t rows_read_internal{0};

	void operator() (const Progress & value)
	{
		merge_entry->bytes_read_uncompressed += value.bytes;
		ProfileEvents::increment(ProfileEvents::MergedUncompressedBytes, value.bytes);

		rows_read_internal += value.rows;
		Float64 local_progress = average_elem_progress * rows_read_internal;
		merge_entry->progress = initial_progress + local_progress;
	};
};

/// parts should be sorted.
MergeTreeData::MutableDataPartPtr MergeTreeDataMerger::mergePartsToTemporaryPart(
	MergeTreeData::DataPartsVector & parts, const String & merged_name, MergeList::Entry & merge_entry,
	size_t aio_threshold, time_t time_of_merge, DiskSpaceMonitor::Reservation * disk_reservation)
{
	if (isCancelled())
		throw Exception("Cancelled merging parts", ErrorCodes::ABORTED);

	merge_entry->num_parts = parts.size();

	LOG_DEBUG(log, "Merging " << parts.size() << " parts: from " << parts.front()->name << " to " << parts.back()->name << " into " << merged_name);

	String merged_dir = data.getFullPath() + merged_name;
	if (Poco::File(merged_dir).exists())
		throw Exception("Directory " + merged_dir + " already exists", ErrorCodes::DIRECTORY_ALREADY_EXISTS);

	for (const MergeTreeData::DataPartPtr & part : parts)
	{
		Poco::ScopedReadRWLock part_lock(part->columns_lock);

		merge_entry->total_size_bytes_compressed += part->size_in_bytes;
		merge_entry->total_size_marks += part->size;
	}

	MergeTreeData::DataPart::ColumnToSize merged_column_to_size;
	for (const MergeTreeData::DataPartPtr & part : parts)
		part->accumulateColumnSizes(merged_column_to_size);

	Names all_column_names = data.getColumnNamesList();
	NamesAndTypesList all_columns = data.getColumnsList();
	SortDescription sort_desc = data.getSortDescription();

	NamesAndTypesList gathering_columns, merging_columns;
	Names gathering_column_names, merging_column_names;
	extractMergingAndGatheringColumns(all_columns, data.getPrimaryExpression(), data.merging_params,
		gathering_columns, gathering_column_names, merging_columns, merging_column_names);

	MergeTreeData::MutableDataPartPtr new_data_part = std::make_shared<MergeTreeData::DataPart>(data);
	ActiveDataPartSet::parsePartName(merged_name, *new_data_part);
	new_data_part->name = "tmp_" + merged_name;
	new_data_part->is_temp = true;

	size_t sum_input_rows_upper_bound = merge_entry->total_size_marks * data.index_granularity;

	MergedRowSources merged_rows_sources;
	MergedRowSources * merged_rows_sources_ptr = &merged_rows_sources;
	MergeAlgorithm merge_alg = chooseMergeAlgorithm(data, parts, sum_input_rows_upper_bound, merged_rows_sources);

	LOG_DEBUG(log, "Selected MergeAlgorithm: " << ((merge_alg == MergeAlgorithm::Vertical) ? "Vertical" : "Horizontal"));

	if (merge_alg != MergeAlgorithm::Vertical)
	{
		merged_rows_sources_ptr = nullptr;
		merging_columns = all_columns;
		merging_column_names = all_column_names;
		gathering_columns.clear();
		gathering_column_names.clear();
	}

	ColumnSizeEstimator column_sizes(merged_column_to_size, merging_column_names, gathering_column_names);

	/** Читаем из всех кусков, сливаем и пишем в новый.
	  * Попутно вычисляем выражение для сортировки.
	  */
	BlockInputStreams src_streams;

	for (size_t i = 0; i < parts.size(); ++i)
	{
		String part_path = data.getFullPath() + parts[i]->name + '/';

		auto input = std::make_unique<MergeTreeBlockInputStream>(
			part_path, DEFAULT_MERGE_BLOCK_SIZE, merging_column_names, data, parts[i],
			MarkRanges(1, MarkRange(0, parts[i]->size)), false, nullptr, "", true, aio_threshold, DBMS_DEFAULT_BUFFER_SIZE, false);

		input->setProgressCallback(MergeProgressCallback{merge_entry, merge_alg, sum_input_rows_upper_bound, column_sizes});

		if (data.merging_params.mode != MergeTreeData::MergingParams::Unsorted)
			src_streams.emplace_back(std::make_shared<MaterializingBlockInputStream>(
				std::make_shared<ExpressionBlockInputStream>(BlockInputStreamPtr(std::move(input)), data.getPrimaryExpression())));
		else
			src_streams.emplace_back(std::move(input));
	}

	/// Порядок потоков важен: при совпадении ключа элементы идут в порядке номера потока-источника.
	/// В слитом куске строки с одинаковым ключом должны идти в порядке возрастания идентификатора исходного куска,
	///  то есть (примерного) возрастания времени вставки.
	std::unique_ptr<IProfilingBlockInputStream> merged_stream;

	switch (data.merging_params.mode)
	{
		case MergeTreeData::MergingParams::Ordinary:
			merged_stream = std::make_unique<MergingSortedBlockInputStream>(
				src_streams, sort_desc, DEFAULT_MERGE_BLOCK_SIZE, 0, merged_rows_sources_ptr);
			break;

		case MergeTreeData::MergingParams::Collapsing:
			merged_stream = std::make_unique<CollapsingSortedBlockInputStream>(
				src_streams, sort_desc, data.merging_params.sign_column, DEFAULT_MERGE_BLOCK_SIZE, merged_rows_sources_ptr);
			break;

		case MergeTreeData::MergingParams::Summing:
			merged_stream = std::make_unique<SummingSortedBlockInputStream>(
				src_streams, sort_desc, data.merging_params.columns_to_sum, DEFAULT_MERGE_BLOCK_SIZE);
			break;

		case MergeTreeData::MergingParams::Aggregating:
			merged_stream = std::make_unique<AggregatingSortedBlockInputStream>(
				src_streams, sort_desc, DEFAULT_MERGE_BLOCK_SIZE);
			break;

		case MergeTreeData::MergingParams::Replacing:
			merged_stream = std::make_unique<ReplacingSortedBlockInputStream>(
				src_streams, sort_desc, data.merging_params.version_column, DEFAULT_MERGE_BLOCK_SIZE);
			break;

		case MergeTreeData::MergingParams::Graphite:
			merged_stream = std::make_unique<GraphiteRollupSortedBlockInputStream>(
				src_streams, sort_desc, DEFAULT_MERGE_BLOCK_SIZE,
				data.merging_params.graphite_params, time_of_merge);
			break;

		case MergeTreeData::MergingParams::Unsorted:
			merged_stream = std::make_unique<ConcatBlockInputStream>(src_streams);
			break;

		default:
			throw Exception("Unknown mode of operation for MergeTreeData: " + toString(data.merging_params.mode), ErrorCodes::LOGICAL_ERROR);
	}

	String new_part_tmp_path = data.getFullPath() + "tmp_" + merged_name + "/";

	auto compression_method = data.context.chooseCompressionMethod(
		merge_entry->total_size_bytes_compressed,
		static_cast<double>(merge_entry->total_size_bytes_compressed) / data.getTotalActiveSizeInBytes());

	MergedBlockOutputStream to{
		data, new_part_tmp_path, merging_columns, compression_method, merged_column_to_size, aio_threshold};

	merged_stream->readPrefix();
	to.writePrefix();

	size_t rows_written = 0;
	const size_t initial_reservation = disk_reservation ? disk_reservation->getSize() : 0;

	Block block;
	while (!isCancelled() && (block = merged_stream->read()))
	{
		rows_written += block.rows();
		to.write(block);

		if (merge_alg == MergeAlgorithm::Horizontal)
			merge_entry->rows_written = merged_stream->getProfileInfo().rows;
		merge_entry->rows_with_key_columns_written = merged_stream->getProfileInfo().rows;
		merge_entry->bytes_written_uncompressed = merged_stream->getProfileInfo().bytes;

		/// This update is unactual for VERTICAL algorithm sicne it requires more accurate per-column updates
		/// Reservation updates is not performed yet, during the merge it may lead to higher free space requirements
		if (disk_reservation && merge_alg == MergeAlgorithm::Horizontal)
		{
			Float64 relative_rows_written = std::min(1., 1. * rows_written / sum_input_rows_upper_bound);
			disk_reservation->update(static_cast<size_t>((1. - relative_rows_written) * initial_reservation));
		}
	}

	if (isCancelled())
		throw Exception("Cancelled merging parts", ErrorCodes::ABORTED);

	MergeTreeData::DataPart::Checksums checksums_ordinary_columns;

	/// Gather ordinary columns
	if (merge_alg == MergeAlgorithm::Vertical)
	{
		size_t sum_input_rows_exact = merge_entry->rows_with_key_columns_read;
		merge_entry->columns_written = merging_column_names.size();
		merge_entry->progress = column_sizes.keyColumnsProgress(sum_input_rows_exact, sum_input_rows_exact);

		BlockInputStreams column_part_streams(parts.size());
		NameSet offset_columns_written;

		auto it_name_and_type = gathering_columns.cbegin();

		for (size_t column_num = 0; column_num < gathering_column_names.size(); ++column_num, it_name_and_type++)
		{
			const String & column_name = it_name_and_type->name;
			const DataTypePtr & column_type = it_name_and_type->type;
			const String offset_column_name = DataTypeNested::extractNestedTableName(column_name);
			Names column_name_(1, column_name);
			NamesAndTypesList column_name_and_type_(1, *it_name_and_type);
			Float64 progress_before = merge_entry->progress;
			bool offset_written = offset_columns_written.count(offset_column_name);

			LOG_TRACE(log, "Gathering column " << column_name <<  " " << column_type->getName());

			for (size_t part_num = 0; part_num < parts.size(); ++part_num)
			{
				String part_path = data.getFullPath() + parts[part_num]->name + '/';

				/// TODO: test perfomance with more accurate settings
				auto column_part_stream = std::make_shared<MergeTreeBlockInputStream>(
					part_path, DEFAULT_MERGE_BLOCK_SIZE, column_name_, data, parts[part_num],
					MarkRanges(1, MarkRange(0, parts[part_num]->size)), false, nullptr, "", true, aio_threshold, DBMS_DEFAULT_BUFFER_SIZE,
					false, true);

				column_part_stream->setProgressCallback(
					MergeProgressCallbackVerticalStep{merge_entry, sum_input_rows_exact, column_sizes, column_name});

				column_part_streams[part_num] = std::move(column_part_stream);
			}

			ColumnGathererStream column_gathered_stream(column_part_streams, column_name, merged_rows_sources, DEFAULT_BLOCK_SIZE);
			MergedColumnOnlyOutputStream column_to(data, new_part_tmp_path, true, compression_method, offset_written);

			column_to.writePrefix();
			while ((block = column_gathered_stream.read()))
			{
				column_to.write(block);
			}
			/// NOTE: nested column contains duplicates checksums (and files)
			checksums_ordinary_columns.add(column_to.writeSuffixAndGetChecksums());

			if (typeid_cast<const DataTypeArray *>(column_type.get()))
				offset_columns_written.emplace(offset_column_name);

			merge_entry->columns_written = merging_column_names.size() + column_num;
			merge_entry->bytes_written_uncompressed += column_gathered_stream.getProfileInfo().bytes;
			merge_entry->progress = progress_before + column_sizes.columnProgress(column_name, sum_input_rows_exact, sum_input_rows_exact);

			if (isCancelled())
				throw Exception("Cancelled merging parts", ErrorCodes::ABORTED);
		}
	}

	merged_stream->readSuffix();
	new_data_part->columns = all_columns;
	if (merge_alg != MergeAlgorithm::Vertical)
		new_data_part->checksums = to.writeSuffixAndGetChecksums();
	else
		new_data_part->checksums = to.writeSuffixAndGetChecksums(all_columns, &checksums_ordinary_columns);
	new_data_part->index.swap(to.getIndex());

	/// Для удобства, даже CollapsingSortedBlockInputStream не может выдать ноль строк.
	if (0 == to.marksCount())
		throw Exception("Empty part after merge", ErrorCodes::LOGICAL_ERROR);

	new_data_part->size = to.marksCount();
	new_data_part->modification_time = time(0);
	new_data_part->size_in_bytes = MergeTreeData::DataPart::calcTotalSize(new_part_tmp_path);
	new_data_part->is_sharded = false;

	return new_data_part;
}


MergeTreeDataMerger::MergeAlgorithm MergeTreeDataMerger::chooseMergeAlgorithm(
	const MergeTreeData & data, const MergeTreeData::DataPartsVector & parts,
	size_t sum_rows_upper_bound, MergedRowSources & rows_sources_to_alloc) const
{
	if (data.context.getMergeTreeSettings().enable_vertical_merge_algorithm == 0)
		return MergeAlgorithm::Horizontal;

	bool is_supported_storage =
		data.merging_params.mode == MergeTreeData::MergingParams::Ordinary ||
		data.merging_params.mode == MergeTreeData::MergingParams::Collapsing;

	bool enough_ordinary_cols = data.getColumnNamesList().size() > data.getSortDescription().size();

	bool enough_total_rows = sum_rows_upper_bound >= DEFAULT_MERGE_BLOCK_SIZE;

	bool no_parts_overflow = parts.size() <= RowSourcePart::MAX_PARTS;

	auto merge_alg = (is_supported_storage && enough_total_rows && enough_ordinary_cols && no_parts_overflow) ?
						MergeAlgorithm::Vertical : MergeAlgorithm::Horizontal;

	if (merge_alg == MergeAlgorithm::Vertical)
	{
		try
		{
			rows_sources_to_alloc.reserve(sum_rows_upper_bound);
		}
		catch (...)
		{
			/// Not enough memory for VERTICAL merge algorithm, make sense for very large tables
			merge_alg = MergeAlgorithm::Horizontal;
		}
	}

	return merge_alg;
}


MergeTreeData::DataPartPtr MergeTreeDataMerger::renameMergedTemporaryPart(
	MergeTreeData::DataPartsVector & parts,
	MergeTreeData::MutableDataPartPtr & new_data_part,
	const String & merged_name,
	MergeTreeData::Transaction * out_transaction)
{
	/// Переименовываем новый кусок, добавляем в набор и убираем исходные куски.
	auto replaced_parts = data.renameTempPartAndReplace(new_data_part, nullptr, out_transaction);

	if (new_data_part->name != merged_name)
		throw Exception("Unexpected part name: " + new_data_part->name + " instead of " + merged_name, ErrorCodes::LOGICAL_ERROR);

	/// Проверим, что удалились все исходные куски и только они.
	if (replaced_parts.size() != parts.size())
	{
		/** Это нормально, хотя такое бывает редко.
		 *
		 * Ситуация - было заменено 0 кусков вместо N может быть, например, в следующем случае:
		 * - у нас был кусок A, но не было куска B и C;
		 * - в очереди был мердж A, B -> AB, но его не делали, так как куска B нет;
		 * - в очереди был мердж AB, C -> ABC, но его не делали, так как куска AB и C нет;
		 * - мы выполнили задачу на скачивание куска B;
		 * - мы начали делать мердж A, B -> AB, так как все куски появились;
		 * - мы решили скачать с другой реплики кусок ABC, так как невозможно было сделать мердж AB, C -> ABC;
		 * - кусок ABC появился, при его добавлении, были удалены старые куски A, B, C;
		 * - мердж AB закончился. Добавился кусок AB. Но это устаревший кусок. В логе будет сообщение Obsolete part added,
		 *   затем попадаем сюда.
		 *
		 * When M > N parts could be replaced?
		 * - new block was added in ReplicatedMergeTreeBlockOutputStream;
		 * - it was added to working dataset in memory and renamed on filesystem;
		 * - but ZooKeeper transaction that add its to reference dataset in ZK and unlocks AbandonableLock is failed;
		 * - and it is failed due to connection loss, so we don't rollback working dataset in memory,
		 *   because we don't know if the part was added to ZK or not
		 *   (see ReplicatedMergeTreeBlockOutputStream)
		 * - then method selectPartsToMerge selects a range and see, that AbandonableLock for this part is abandoned,
		 *   and so, it is possible to merge a range skipping this part.
		 *   (NOTE: Merging with part that is not in ZK is not possible, see checks in 'createLogEntryToMergeParts'.)
		 * - and after merge, this part will be removed in addition to parts that was merged.
		 */
		LOG_WARNING(log, "Unexpected number of parts removed when adding " << new_data_part->name << ": " << replaced_parts.size()
			<< " instead of " << parts.size());
	}
	else
	{
		for (size_t i = 0; i < parts.size(); ++i)
			if (parts[i]->name != replaced_parts[i]->name)
				throw Exception("Unexpected part removed when adding " + new_data_part->name + ": " + replaced_parts[i]->name
					+ " instead of " + parts[i]->name, ErrorCodes::LOGICAL_ERROR);
	}

	LOG_TRACE(log, "Merged " << parts.size() << " parts: from " << parts.front()->name << " to " << parts.back()->name);
	return new_data_part;
}


MergeTreeData::PerShardDataParts MergeTreeDataMerger::reshardPartition(
	const ReshardingJob & job, DiskSpaceMonitor::Reservation * disk_reservation)
{
	size_t aio_threshold = data.context.getSettings().min_bytes_to_use_direct_io;

	/// Собрать все куски партиции.
	DayNum_t month = MergeTreeData::getMonthFromName(job.partition);
	MergeTreeData::DataPartsVector parts = selectAllPartsFromPartition(month);

	/// Создать временное название папки.
	std::string merged_name = createMergedPartName(parts);

	MergeList::EntryPtr merge_entry_ptr = data.context.getMergeList().insert(job.database_name,
		job.table_name, merged_name);
	MergeList::Entry & merge_entry = *merge_entry_ptr;
	merge_entry->num_parts = parts.size();

	LOG_DEBUG(log, "Resharding " << parts.size() << " parts from " << parts.front()->name
		<< " to " << parts.back()->name << " which span the partition " << job.partition);

	/// Слияние всех кусков партиции.

	for (const MergeTreeData::DataPartPtr & part : parts)
	{
		Poco::ScopedReadRWLock part_lock(part->columns_lock);

		merge_entry->total_size_bytes_compressed += part->size_in_bytes;
		merge_entry->total_size_marks += part->size;
	}

	MergeTreeData::DataPart::ColumnToSize merged_column_to_size;
	if (aio_threshold > 0)
	{
		for (const MergeTreeData::DataPartPtr & part : parts)
			part->accumulateColumnSizes(merged_column_to_size);
	}

	Names column_names = data.getColumnNamesList();
	NamesAndTypesList column_names_and_types = data.getColumnsList();

	BlockInputStreams src_streams;

	size_t sum_rows_approx = 0;

	const auto rows_total = merge_entry->total_size_marks * data.index_granularity;

	for (size_t i = 0; i < parts.size(); ++i)
	{
		MarkRanges ranges(1, MarkRange(0, parts[i]->size));

		String part_path = data.getFullPath() + parts[i]->name + '/';

		auto input = std::make_unique<MergeTreeBlockInputStream>(
			part_path, DEFAULT_MERGE_BLOCK_SIZE, column_names, data,
			parts[i], ranges, false, nullptr, "", true, aio_threshold, DBMS_DEFAULT_BUFFER_SIZE, false);

		input->setProgressCallback([&merge_entry, rows_total] (const Progress & value)
			{
				const auto new_rows_read = merge_entry->rows_read += value.rows;
				merge_entry->progress = static_cast<Float64>(new_rows_read) / rows_total;
				merge_entry->bytes_read_uncompressed += value.bytes;
			});

		if (data.merging_params.mode != MergeTreeData::MergingParams::Unsorted)
			src_streams.emplace_back(std::make_shared<MaterializingBlockInputStream>(
				std::make_shared<ExpressionBlockInputStream>(BlockInputStreamPtr(std::move(input)), data.getPrimaryExpression())));
		else
			src_streams.emplace_back(std::move(input));

		sum_rows_approx += parts[i]->size * data.index_granularity;
	}

	/// Шардирование слитых блоков.

	/// Для нумерации блоков.
	SimpleIncrement increment(job.block_number);

	/// Создать новый кусок для каждого шарда.
	MergeTreeData::PerShardDataParts per_shard_data_parts;

	per_shard_data_parts.reserve(job.paths.size());
	for (size_t shard_no = 0; shard_no < job.paths.size(); ++shard_no)
	{
		Int64 temp_index = increment.get();

		MergeTreeData::MutableDataPartPtr data_part = std::make_shared<MergeTreeData::DataPart>(data);
		data_part->name = "tmp_" + merged_name;
		data_part->is_temp = true;
		data_part->left_date = std::numeric_limits<UInt16>::max();
		data_part->right_date = std::numeric_limits<UInt16>::min();
		data_part->month = month;
		data_part->left = temp_index;
		data_part->right = temp_index;
		data_part->level = 0;
		per_shard_data_parts.emplace(shard_no, data_part);
	}

	/// Очень грубая оценка для размера сжатых данных каждой шардированной партиции.
	/// На самом деле всё зависит от свойств выражения для шардирования.
	UInt64 per_shard_size_bytes_compressed = merge_entry->total_size_bytes_compressed / static_cast<double>(job.paths.size());

	auto compression_method = data.context.chooseCompressionMethod(
		per_shard_size_bytes_compressed,
		static_cast<double>(per_shard_size_bytes_compressed) / data.getTotalActiveSizeInBytes());

	using MergedBlockOutputStreamPtr = std::unique_ptr<MergedBlockOutputStream>;
	using PerShardOutput = std::unordered_map<size_t, MergedBlockOutputStreamPtr>;

	/// Создать для каждого шарда поток, который записывает соответствующие шардированные блоки.
	PerShardOutput per_shard_output;

	per_shard_output.reserve(job.paths.size());
	for (size_t shard_no = 0; shard_no < job.paths.size(); ++shard_no)
	{
		std::string new_part_tmp_path = data.getFullPath() + "reshard/" + toString(shard_no) + "/tmp_" + merged_name + "/";
		Poco::File(new_part_tmp_path).createDirectories();

		MergedBlockOutputStreamPtr output_stream;
		output_stream = std::make_unique<MergedBlockOutputStream>(
			data, new_part_tmp_path, column_names_and_types, compression_method, merged_column_to_size, aio_threshold);
		per_shard_output.emplace(shard_no, std::move(output_stream));
	}

	/// Порядок потоков важен: при совпадении ключа элементы идут в порядке номера потока-источника.
	/// В слитом куске строки с одинаковым ключом должны идти в порядке возрастания идентификатора исходного куска,
	///  то есть (примерного) возрастания времени вставки.
	std::unique_ptr<IProfilingBlockInputStream> merged_stream;

	switch (data.merging_params.mode)
	{
		case MergeTreeData::MergingParams::Ordinary:
			merged_stream = std::make_unique<MergingSortedBlockInputStream>(
				src_streams, data.getSortDescription(), DEFAULT_MERGE_BLOCK_SIZE);
			break;

		case MergeTreeData::MergingParams::Collapsing:
			merged_stream = std::make_unique<CollapsingSortedBlockInputStream>(
				src_streams, data.getSortDescription(), data.merging_params.sign_column, DEFAULT_MERGE_BLOCK_SIZE);
			break;

		case MergeTreeData::MergingParams::Summing:
			merged_stream = std::make_unique<SummingSortedBlockInputStream>(
				src_streams, data.getSortDescription(), data.merging_params.columns_to_sum, DEFAULT_MERGE_BLOCK_SIZE);
			break;

		case MergeTreeData::MergingParams::Aggregating:
			merged_stream = std::make_unique<AggregatingSortedBlockInputStream>(
				src_streams, data.getSortDescription(), DEFAULT_MERGE_BLOCK_SIZE);
			break;

		case MergeTreeData::MergingParams::Replacing:
			merged_stream = std::make_unique<ReplacingSortedBlockInputStream>(
				src_streams, data.getSortDescription(), data.merging_params.version_column, DEFAULT_MERGE_BLOCK_SIZE);
			break;

		case MergeTreeData::MergingParams::Graphite:
			merged_stream = std::make_unique<GraphiteRollupSortedBlockInputStream>(
				src_streams, data.getSortDescription(), DEFAULT_MERGE_BLOCK_SIZE,
				data.merging_params.graphite_params, time(0));
			break;

		case MergeTreeData::MergingParams::Unsorted:
			merged_stream = std::make_unique<ConcatBlockInputStream>(src_streams);
			break;

		default:
			throw Exception("Unknown mode of operation for MergeTreeData: " + toString(data.merging_params.mode), ErrorCodes::LOGICAL_ERROR);
	}

	merged_stream->readPrefix();

	for (auto & entry : per_shard_output)
	{
		MergedBlockOutputStreamPtr & output_stream = entry.second;
		output_stream->writePrefix();
	}

	size_t rows_written = 0;
	const size_t initial_reservation = disk_reservation ? disk_reservation->getSize() : 0;

	MergeTreeSharder sharder(data, job);

	while (Block block = merged_stream->read())
	{
		abortReshardPartitionIfRequested();

		ShardedBlocksWithDateIntervals blocks = sharder.shardBlock(block);

		for (ShardedBlockWithDateInterval & block_with_dates : blocks)
		{
			abortReshardPartitionIfRequested();

			size_t shard_no = block_with_dates.shard_no;
			MergeTreeData::MutableDataPartPtr & data_part = per_shard_data_parts.at(shard_no);
			MergedBlockOutputStreamPtr & output_stream = per_shard_output.at(shard_no);

			rows_written += block_with_dates.block.rows();
			output_stream->write(block_with_dates.block);

			if (block_with_dates.min_date < data_part->left_date)
				data_part->left_date = block_with_dates.min_date;
			if (block_with_dates.max_date > data_part->right_date)
				data_part->right_date = block_with_dates.max_date;

			merge_entry->rows_written = merged_stream->getProfileInfo().rows;
			merge_entry->bytes_written_uncompressed = merged_stream->getProfileInfo().bytes;

			if (disk_reservation)
				disk_reservation->update(static_cast<size_t>((1 - std::min(1., 1. * rows_written / sum_rows_approx)) * initial_reservation));
		}
	}

	merged_stream->readSuffix();

	/// Завершить инициализацию куски новых партиций.
	for (size_t shard_no = 0; shard_no < job.paths.size(); ++shard_no)
	{
		abortReshardPartitionIfRequested();

		MergedBlockOutputStreamPtr & output_stream = per_shard_output.at(shard_no);
		if (0 == output_stream->marksCount())
		{
			/// В этот шард не попало никаких данных. Игнорируем.
			LOG_WARNING(log, "No data in partition for shard " + job.paths[shard_no].first);
			per_shard_data_parts.erase(shard_no);
			continue;
		}

		MergeTreeData::MutableDataPartPtr & data_part = per_shard_data_parts.at(shard_no);

		data_part->columns = column_names_and_types;
		data_part->checksums = output_stream->writeSuffixAndGetChecksums();
		data_part->index.swap(output_stream->getIndex());
		data_part->size = output_stream->marksCount();
		data_part->modification_time = time(0);
		data_part->size_in_bytes = MergeTreeData::DataPart::calcTotalSize(output_stream->getPartPath());
		data_part->is_sharded = true;
		data_part->shard_no = shard_no;
	}

	/// Превратить куски новых партиций в постоянные куски.
	for (auto & entry : per_shard_data_parts)
	{
		size_t shard_no = entry.first;
		MergeTreeData::MutableDataPartPtr & part_from_shard = entry.second;
		part_from_shard->is_temp = false;
		std::string prefix = data.getFullPath() + "reshard/" + toString(shard_no) + "/";
		std::string old_name = part_from_shard->name;
		std::string new_name = ActiveDataPartSet::getPartName(part_from_shard->left_date,
			part_from_shard->right_date, part_from_shard->left, part_from_shard->right, part_from_shard->level);
		part_from_shard->name = new_name;
		Poco::File(prefix + old_name).renameTo(prefix + new_name);
	}

	LOG_TRACE(log, "Resharded the partition " << job.partition);

	return per_shard_data_parts;
}

size_t MergeTreeDataMerger::estimateDiskSpaceForMerge(const MergeTreeData::DataPartsVector & parts)
{
	size_t res = 0;
	for (const MergeTreeData::DataPartPtr & part : parts)
		res += part->size_in_bytes;

	return static_cast<size_t>(res * DISK_USAGE_COEFFICIENT_TO_RESERVE);
}

void MergeTreeDataMerger::abortReshardPartitionIfRequested()
{
	if (isCancelled())
		throw Exception("Cancelled partition resharding", ErrorCodes::ABORTED);

	if (cancellation_hook)
		cancellation_hook();
}

}
