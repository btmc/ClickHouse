#include <DB/Functions/FunctionFactory.h>
#include <DB/Functions/FunctionsArray.h>

namespace DB
{

void FunctionArray::execute(Block & block, const ColumnNumbers & arguments, size_t result)
{
	size_t num_elements = arguments.size();
	bool is_const = true;

	for (const auto arg_num : arguments)
	{
		if (!block.getByPosition(arg_num).column->isConst())
		{
			is_const = false;
			break;
		}
	}

	const auto first_arg = block.getByPosition(arguments[0]);
	DataTypePtr result_type = first_arg.type;
	DataTypeTraits::EnrichedDataTypePtr enriched_result_type;
	if (result_type->behavesAsNumber())
	{
		/// If type is numeric, calculate least common type.
		DataTypes types;
		types.reserve(num_elements);

		for (const auto & argument : arguments)
			types.push_back(block.getByPosition(argument).type);

		enriched_result_type = getLeastCommonType(types);
		result_type = enriched_result_type.first;
	}

	if (is_const)
	{
		Array arr;
		for (const auto arg_num : arguments)
			if (block.getByPosition(arg_num).type->getName() == result_type->getName())
				/// Если элемент такого же типа как результат, просто добавляем его в ответ
				arr.push_back((*block.getByPosition(arg_num).column)[0]);
			else
				/// Иначе необходимо привести его к типу результата
				addField(result_type, (*block.getByPosition(arg_num).column)[0], arr);

		block.getByPosition(result).column = std::make_shared<ColumnConstArray>(
			first_arg.column->size(), arr, std::make_shared<DataTypeArray>(result_type));
	}
	else
	{
		size_t block_size = block.rowsInFirstColumn();

		/** If part of columns have not same type as common type of all elements of array,
			*  then convert them to common type.
			* If part of columns are constants,
			*  then convert them to full columns.
			*/

		Columns columns_holder(num_elements);
		const IColumn * columns[num_elements];

		for (size_t i = 0; i < num_elements; ++i)
		{
			const auto & arg = block.getByPosition(arguments[i]);

			String result_type_name = result_type->getName();
			ColumnPtr preprocessed_column = arg.column;

			if (arg.type->getName() != result_type_name)
			{
				Block temporary_block
				{
					{
						arg.column,
						arg.type,
						arg.name
					},
					{
						std::make_shared<ColumnConstString>(block_size, result_type_name),
						std::make_shared<DataTypeString>(),
						""
					},
					{
						nullptr,
						result_type,
						""
					}
				};

				FunctionCast func_cast(context);

				{
					DataTypePtr unused_return_type;
					ColumnsWithTypeAndName arguments{ temporary_block.unsafeGetByPosition(0), temporary_block.unsafeGetByPosition(1) };
					std::vector<ExpressionAction> unused_prerequisites;

					/// Prepares function to execution. TODO It is not obvious.
					func_cast.getReturnTypeAndPrerequisites(arguments, unused_return_type, unused_prerequisites);
				}

				func_cast.execute(temporary_block, {0, 1}, 2);
				preprocessed_column = temporary_block.unsafeGetByPosition(2).column;
			}

			if (auto materialized_column = preprocessed_column->convertToFullColumnIfConst())
				preprocessed_column = materialized_column;

			columns_holder[i] = std::move(preprocessed_column);
			columns[i] = columns_holder[i].get();
		}

		/** Create and fill the result array.
			*/

		auto out = std::make_shared<ColumnArray>(result_type->createColumn());
		IColumn & out_data = out->getData();
		IColumn::Offsets_t & out_offsets = out->getOffsets();

		out_data.reserve(block_size * num_elements);
		out_offsets.resize(block_size);

		IColumn::Offset_t current_offset = 0;
		for (size_t i = 0; i < block_size; ++i)
		{
			for (size_t j = 0; j < num_elements; ++j)
				out_data.insertFrom(*columns[j], i);

			current_offset += num_elements;
			out_offsets[i] = current_offset;
		}

		block.getByPosition(result).column = out;
	}
}


DataTypePtr FunctionArray::getReturnType(const DataTypes & arguments) const
{
	if (arguments.empty())
	{
		if (is_case_mode)
			throw Exception{"Either WHEN clauses or THEN clauses are missing "
				"in the CASE construction.", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};
		else
			throw Exception{"Function array requires at least one argument.", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};
	}

	DataTypePtr result_type = arguments[0];

	if (result_type->behavesAsNumber())
	{
		/// Если тип числовой, пробуем выделить наименьший общий тип
		auto enriched_result_type = getLeastCommonType(arguments);
		return std::make_shared<DataTypeArray>(enriched_result_type);
	}
	else
	{
		/// Иначе все аргументы должны быть одинаковыми
		for (size_t i = 1, size = arguments.size(); i < size; ++i)
		{
			if (arguments[i]->getName() != arguments[0]->getName())
			{
				if (is_case_mode)
					throw Exception{"Found type discrepancy in either WHEN "
						"clauses or THEN clauses of the CASE construction",
						ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
				else
					throw Exception{"Arguments for function array must have same type or behave as number.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
			}
		}

		return std::make_shared<DataTypeArray>(result_type);
	}
}


void FunctionArray::setCaseMode()
{
	is_case_mode = true;
}


String FunctionArray::getName() const
{
	return is_case_mode ? "CASE" : name;
}


template <typename T0, typename T1>
bool FunctionArray::tryAddField(DataTypePtr type_res, const Field & f, Array & arr) const
{
	if (typeid_cast<const T0 *>(type_res.get()))
	{
		arr.push_back(apply_visitor(FieldVisitorConvertToNumber<typename T1::FieldType>(), f));
		return true;
	}
	return false;
}


bool FunctionArray::addField(DataTypePtr type_res, const Field & f, Array & arr) const
{
	/// Иначе необходимо
	if (	tryAddField<DataTypeUInt8, DataTypeUInt64>(type_res, f, arr)
		||	tryAddField<DataTypeUInt16, DataTypeUInt64>(type_res, f, arr)
		||	tryAddField<DataTypeUInt32, DataTypeUInt64>(type_res, f, arr)
		||	tryAddField<DataTypeUInt64, DataTypeUInt64>(type_res, f, arr)
		||	tryAddField<DataTypeInt8, DataTypeInt64>(type_res, f, arr)
		||	tryAddField<DataTypeInt16, DataTypeInt64>(type_res, f, arr)
		||	tryAddField<DataTypeInt32, DataTypeInt64>(type_res, f, arr)
		||	tryAddField<DataTypeInt64, DataTypeInt64>(type_res, f, arr)
		||	tryAddField<DataTypeFloat32, DataTypeFloat64>(type_res, f, arr)
		||	tryAddField<DataTypeFloat64, DataTypeFloat64>(type_res, f, arr) )
		return true;
	else
	{
		if (is_case_mode)
			throw Exception{"Illegal type encountered while processing the CASE construction.",
				ErrorCodes::LOGICAL_ERROR};
		else
			throw Exception{"Illegal result type " + type_res->getName() + " of function " + getName(),
							ErrorCodes::LOGICAL_ERROR};
	}
}


DataTypeTraits::EnrichedDataTypePtr FunctionArray::getLeastCommonType(const DataTypes & arguments) const
{
	DataTypeTraits::EnrichedDataTypePtr result_type;

	try
	{
		result_type = Conditional::getArrayType(arguments);
	}
	catch (const Conditional::CondException & ex)
	{
		/// Translate a context-free error into a contextual error.
		if (is_case_mode)
		{
			if (ex.getCode() == Conditional::CondErrorCodes::TYPE_DEDUCER_ILLEGAL_COLUMN_TYPE)
				throw Exception{"Illegal type of column " + ex.getMsg1() +
					" in CASE construction", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
			else if (ex.getCode() == Conditional::CondErrorCodes::TYPE_DEDUCER_UPSCALING_ERROR)
				throw Exception{"THEN/ELSE clause parameters in CASE construction are not upscalable to a "
					"common type without loss of precision: " + ex.getMsg1(),
					ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
			else
				throw Exception{"An unexpected error has occurred in CASE expression",
					ErrorCodes::LOGICAL_ERROR};
		}
		else
		{
			if (ex.getCode() == Conditional::CondErrorCodes::TYPE_DEDUCER_ILLEGAL_COLUMN_TYPE)
				throw Exception{"Illegal type of column " + ex.getMsg1() +
					" in array", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
			else if (ex.getCode() == Conditional::CondErrorCodes::TYPE_DEDUCER_UPSCALING_ERROR)
				throw Exception("Arguments of function " + getName() + " are not upscalable "
					"to a common type without loss of precision.",
					ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
			else
				throw Exception{"An unexpected error has occurred in function " + getName(),
					ErrorCodes::LOGICAL_ERROR};
		}
	}

	return result_type;
}



String FunctionArrayElement::getName() const
{
	return name;
}


DataTypePtr FunctionArrayElement::getReturnType(const DataTypes & arguments) const
{
	if (arguments.size() != 2)
		throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
			+ toString(arguments.size()) + ", should be 2.",
			ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

	const DataTypeArray * array_type = typeid_cast<const DataTypeArray *>(arguments[0].get());
	if (!array_type)
		throw Exception("First argument for function " + getName() + " must be array.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

	if (!arguments[1]->isNumeric()
		|| (!startsWith(arguments[1]->getName(), "UInt") && !startsWith(arguments[1]->getName(), "Int")))
		throw Exception("Second argument for function " + getName() + " must have UInt or Int type.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

	return array_type->getNestedType();
}


void FunctionArrayElement::execute(Block & block, const ColumnNumbers & arguments, size_t result)
{
	if (executeTuple(block, arguments, result))
	{
	}
	else if (!block.getByPosition(arguments[1]).column->isConst())
	{
		if (!(	executeArgument<UInt8>	(block, arguments, result)
			||	executeArgument<UInt16>	(block, arguments, result)
			||	executeArgument<UInt32>	(block, arguments, result)
			||	executeArgument<UInt64>	(block, arguments, result)
			||	executeArgument<Int8>	(block, arguments, result)
			||	executeArgument<Int16>	(block, arguments, result)
			||	executeArgument<Int32>	(block, arguments, result)
			||	executeArgument<Int64>	(block, arguments, result)))
		throw Exception("Second argument for function " + getName() + " must must have UInt or Int type.",
						ErrorCodes::ILLEGAL_COLUMN);
	}
	else
	{
		Field index = (*block.getByPosition(arguments[1]).column)[0];

		if (index == UInt64(0))
			throw Exception("Array indices is 1-based", ErrorCodes::ZERO_ARRAY_OR_TUPLE_INDEX);

		if (!(	executeNumberConst<UInt8>	(block, arguments, result, index)
			||	executeNumberConst<UInt16>	(block, arguments, result, index)
			||	executeNumberConst<UInt32>	(block, arguments, result, index)
			||	executeNumberConst<UInt64>	(block, arguments, result, index)
			||	executeNumberConst<Int8>		(block, arguments, result, index)
			||	executeNumberConst<Int16>	(block, arguments, result, index)
			||	executeNumberConst<Int32>	(block, arguments, result, index)
			||	executeNumberConst<Int64>	(block, arguments, result, index)
			||	executeNumberConst<Float32>	(block, arguments, result, index)
			||	executeNumberConst<Float64>	(block, arguments, result, index)
			||	executeConstConst			(block, arguments, result, index)
			||	executeStringConst			(block, arguments, result, index)
			||	executeGenericConst			(block, arguments, result, index)))
		throw Exception("Illegal column " + block.getByPosition(arguments[0]).column->getName()
			+ " of first argument of function " + getName(),
			ErrorCodes::ILLEGAL_COLUMN);
	}
}


template <typename DataType>
bool FunctionArrayElement::executeNumberConst(Block & block, const ColumnNumbers & arguments, size_t result, const Field & index)
{
	const ColumnArray * col_array = typeid_cast<const ColumnArray *>(block.getByPosition(arguments[0]).column.get());

	if (!col_array)
		return false;

	const ColumnVector<DataType> * col_nested = typeid_cast<const ColumnVector<DataType> *>(&col_array->getData());

	if (!col_nested)
		return false;

	auto col_res = std::make_shared<ColumnVector<DataType>>();
	block.getByPosition(result).column = col_res;

	if (index.getType() == Field::Types::UInt64)
		ArrayElementNumImpl<DataType>::template vectorConst<false>(
			col_nested->getData(), col_array->getOffsets(), safeGet<UInt64>(index) - 1, col_res->getData());
	else if (index.getType() == Field::Types::Int64)
		ArrayElementNumImpl<DataType>::template vectorConst<true>(
			col_nested->getData(), col_array->getOffsets(), -safeGet<Int64>(index) - 1, col_res->getData());
	else
		throw Exception("Illegal type of array index", ErrorCodes::LOGICAL_ERROR);

	return true;
}


template <typename IndexType, typename DataType>
bool FunctionArrayElement::executeNumber(Block & block, const ColumnNumbers & arguments, size_t result, const PaddedPODArray<IndexType> & indices)
{
	const ColumnArray * col_array = typeid_cast<const ColumnArray *>(block.getByPosition(arguments[0]).column.get());

	if (!col_array)
		return false;

	const ColumnVector<DataType> * col_nested = typeid_cast<const ColumnVector<DataType> *>(&col_array->getData());

	if (!col_nested)
		return false;

	auto col_res = std::make_shared<ColumnVector<DataType>>();
	block.getByPosition(result).column = col_res;

	ArrayElementNumImpl<DataType>::template vector<IndexType>(
		col_nested->getData(), col_array->getOffsets(), indices, col_res->getData());

	return true;
}


bool FunctionArrayElement::executeStringConst(Block & block, const ColumnNumbers & arguments, size_t result, const Field & index)
{
	const ColumnArray * col_array = typeid_cast<const ColumnArray *>(block.getByPosition(arguments[0]).column.get());

	if (!col_array)
		return false;

	const ColumnString * col_nested = typeid_cast<const ColumnString *>(&col_array->getData());

	if (!col_nested)
		return false;

	std::shared_ptr<ColumnString> col_res = std::make_shared<ColumnString>();
	block.getByPosition(result).column = col_res;

	if (index.getType() == Field::Types::UInt64)
		ArrayElementStringImpl::vectorConst<false>(
			col_nested->getChars(),
			col_array->getOffsets(),
			col_nested->getOffsets(),
			safeGet<UInt64>(index) - 1,
			col_res->getChars(),
			col_res->getOffsets());
	else if (index.getType() == Field::Types::Int64)
		ArrayElementStringImpl::vectorConst<true>(
			col_nested->getChars(),
			col_array->getOffsets(),
			col_nested->getOffsets(),
			-safeGet<Int64>(index) - 1,
			col_res->getChars(),
			col_res->getOffsets());
	else
		throw Exception("Illegal type of array index", ErrorCodes::LOGICAL_ERROR);

	return true;
}


template <typename IndexType>
bool FunctionArrayElement::executeString(Block & block, const ColumnNumbers & arguments, size_t result, const PaddedPODArray<IndexType> & indices)
{
	const ColumnArray * col_array = typeid_cast<const ColumnArray *>(block.getByPosition(arguments[0]).column.get());

	if (!col_array)
		return false;

	const ColumnString * col_nested = typeid_cast<const ColumnString *>(&col_array->getData());

	if (!col_nested)
		return false;

	std::shared_ptr<ColumnString> col_res = std::make_shared<ColumnString>();
	block.getByPosition(result).column = col_res;

	ArrayElementStringImpl::vector<IndexType>(
		col_nested->getChars(),
		col_array->getOffsets(),
		col_nested->getOffsets(),
		indices,
		col_res->getChars(),
		col_res->getOffsets());

	return true;
}


bool FunctionArrayElement::executeGenericConst(Block & block, const ColumnNumbers & arguments, size_t result, const Field & index)
{
	const ColumnArray * col_array = typeid_cast<const ColumnArray *>(block.getByPosition(arguments[0]).column.get());

	if (!col_array)
		return false;

	const auto & col_nested = col_array->getData();
	auto col_res = col_nested.cloneEmpty();
	block.getByPosition(result).column = col_res;

	if (index.getType() == Field::Types::UInt64)
		ArrayElementGenericImpl::vectorConst<false>(
			col_nested, col_array->getOffsets(), safeGet<UInt64>(index) - 1, *col_res);
	else if (index.getType() == Field::Types::Int64)
		ArrayElementGenericImpl::vectorConst<true>(
			col_nested, col_array->getOffsets(), -safeGet<Int64>(index) - 1, *col_res);
	else
		throw Exception("Illegal type of array index", ErrorCodes::LOGICAL_ERROR);

	return true;
}


template <typename IndexType>
bool FunctionArrayElement::executeGeneric(Block & block, const ColumnNumbers & arguments, size_t result, const PaddedPODArray<IndexType> & indices)
{
	const ColumnArray * col_array = typeid_cast<const ColumnArray *>(block.getByPosition(arguments[0]).column.get());

	if (!col_array)
		return false;

	const auto & col_nested = col_array->getData();
	auto col_res = col_nested.cloneEmpty();
	block.getByPosition(result).column = col_res;

	ArrayElementGenericImpl::vector<IndexType>(
		col_nested, col_array->getOffsets(), indices, *col_res);

	return true;
}


bool FunctionArrayElement::executeConstConst(Block & block, const ColumnNumbers & arguments, size_t result, const Field & index)
{
	const ColumnConstArray * col_array = typeid_cast<const ColumnConstArray *>(block.getByPosition(arguments[0]).column.get());

	if (!col_array)
		return false;

	const DB::Array & array = col_array->getData();
	size_t array_size = array.size();
	size_t real_index = 0;

	if (index.getType() == Field::Types::UInt64)
		real_index = safeGet<UInt64>(index) - 1;
	else if (index.getType() == Field::Types::Int64)
		real_index = array_size + safeGet<Int64>(index);
	else
		throw Exception("Illegal type of array index", ErrorCodes::LOGICAL_ERROR);

	Field value = col_array->getData().at(real_index);

	block.getByPosition(result).column = block.getByPosition(result).type->createConstColumn(
		block.rowsInFirstColumn(),
		value);

	return true;
}


template <typename IndexType>
bool FunctionArrayElement::executeConst(Block & block, const ColumnNumbers & arguments, size_t result, const PaddedPODArray<IndexType> & indices)
{
	const ColumnConstArray * col_array = typeid_cast<const ColumnConstArray *>(block.getByPosition(arguments[0]).column.get());

	if (!col_array)
		return false;

	const DB::Array & array = col_array->getData();
	size_t array_size = array.size();

	block.getByPosition(result).column = block.getByPosition(result).type->createColumn();

	for (size_t i = 0; i < col_array->size(); ++i)
	{
		IndexType index = indices[i];
		if (index > 0 && static_cast<size_t>(index) <= array_size)
			block.getByPosition(result).column->insert(array[index - 1]);
		else if (index < 0 && static_cast<size_t>(-index) <= array_size)
			block.getByPosition(result).column->insert(array[array_size + index]);
		else
			block.getByPosition(result).column->insertDefault();
	}

	return true;
}


template <typename IndexType>
bool FunctionArrayElement::executeArgument(Block & block, const ColumnNumbers & arguments, size_t result)
{
	auto index = typeid_cast<const ColumnVector<IndexType> *>(block.getByPosition(arguments[1]).column.get());

	if (!index)
		return false;

	const auto & index_data = index->getData();

	if (!(	executeNumber<IndexType, UInt8>		(block, arguments, result, index_data)
		||	executeNumber<IndexType, UInt16>	(block, arguments, result, index_data)
		||	executeNumber<IndexType, UInt32>	(block, arguments, result, index_data)
		||	executeNumber<IndexType, UInt64>	(block, arguments, result, index_data)
		||	executeNumber<IndexType, Int8>		(block, arguments, result, index_data)
		||	executeNumber<IndexType, Int16>		(block, arguments, result, index_data)
		||	executeNumber<IndexType, Int32>		(block, arguments, result, index_data)
		||	executeNumber<IndexType, Int64>		(block, arguments, result, index_data)
		||	executeNumber<IndexType, Float32>	(block, arguments, result, index_data)
		||	executeNumber<IndexType, Float64>	(block, arguments, result, index_data)
		||	executeConst <IndexType>			(block, arguments, result, index_data)
		||	executeString<IndexType>			(block, arguments, result, index_data)
		||	executeGeneric<IndexType>			(block, arguments, result, index_data)))
	throw Exception("Illegal column " + block.getByPosition(arguments[0]).column->getName()
				+ " of first argument of function " + getName(), ErrorCodes::ILLEGAL_COLUMN);

	return true;
}


bool FunctionArrayElement::executeTuple(Block & block, const ColumnNumbers & arguments, size_t result)
{
	ColumnArray * col_array = typeid_cast<ColumnArray *>(block.getByPosition(arguments[0]).column.get());

	if (!col_array)
		return false;

	ColumnTuple * col_nested = typeid_cast<ColumnTuple *>(&col_array->getData());

	if (!col_nested)
		return false;

	Block & tuple_block = col_nested->getData();
	size_t tuple_size = tuple_block.columns();

	/** Будем вычислять функцию для кортежа внутренностей массива.
		* Для этого создадим временный блок.
		* Он будет состоять из следующих столбцов:
		* - индекс массива, который нужно взять;
		* - массив из первых элементов кортежей;
		* - результат взятия элементов по индексу для массива из первых элементов кортежей;
		* - массив из вторых элементов кортежей;
		* - результат взятия элементов по индексу для массива из вторых элементов кортежей;
		* ...
		*/
	Block block_of_temporary_results;
	block_of_temporary_results.insert(block.getByPosition(arguments[1]));

	/// результаты взятия элементов по индексу для массивов из каждых элементов кортежей;
	Block result_tuple_block;

	for (size_t i = 0; i < tuple_size; ++i)
	{
		ColumnWithTypeAndName array_of_tuple_section;
		array_of_tuple_section.column = std::make_shared<ColumnArray>(
			tuple_block.getByPosition(i).column, col_array->getOffsetsColumn());
		array_of_tuple_section.type = std::make_shared<DataTypeArray>(
			tuple_block.getByPosition(i).type);
		block_of_temporary_results.insert(array_of_tuple_section);

		ColumnWithTypeAndName array_elements_of_tuple_section;
		block_of_temporary_results.insert(array_elements_of_tuple_section);

		execute(block_of_temporary_results, ColumnNumbers{i * 2 + 1, 0}, i * 2 + 2);

		result_tuple_block.insert(block_of_temporary_results.getByPosition(i * 2 + 2));
	}

	auto col_res = std::make_shared<ColumnTuple>(result_tuple_block);
	block.getByPosition(result).column = col_res;

	return true;
}



String FunctionArrayEnumerate::getName() const
{
	return name;
}


DataTypePtr FunctionArrayEnumerate::getReturnType(const DataTypes & arguments) const
{
	if (arguments.size() != 1)
		throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
			+ toString(arguments.size()) + ", should be 1.",
			ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

	const DataTypeArray * array_type = typeid_cast<const DataTypeArray *>(arguments[0].get());
	if (!array_type)
		throw Exception("First argument for function " + getName() + " must be an array but it has type "
			+ arguments[0]->getName() + ".", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

	return std::make_shared<DataTypeArray>(std::make_shared<DataTypeUInt32>());
}


void FunctionArrayEnumerate::execute(Block & block, const ColumnNumbers & arguments, size_t result)
{
	if (const ColumnArray * array = typeid_cast<const ColumnArray *>(block.getByPosition(arguments[0]).column.get()))
	{
		const ColumnArray::Offsets_t & offsets = array->getOffsets();

		auto res_nested = std::make_shared<ColumnUInt32>();
		auto res_array = std::make_shared<ColumnArray>(res_nested, array->getOffsetsColumn());
		block.getByPosition(result).column = res_array;

		ColumnUInt32::Container_t & res_values = res_nested->getData();
		res_values.resize(array->getData().size());
		size_t prev_off = 0;
		for (size_t i = 0; i < offsets.size(); ++i)
		{
			size_t off = offsets[i];
			for (size_t j = prev_off; j < off; ++j)
			{
				res_values[j] = j - prev_off + 1;
			}
			prev_off = off;
		}
	}
	else if (const ColumnConstArray * array = typeid_cast<const ColumnConstArray *>(block.getByPosition(arguments[0]).column.get()))
	{
		const Array & values = array->getData();

		Array res_values(values.size());
		for (size_t i = 0; i < values.size(); ++i)
		{
			res_values[i] = i + 1;
		}

		auto res_array = std::make_shared<ColumnConstArray>(array->size(), res_values, std::make_shared<DataTypeArray>(std::make_shared<DataTypeUInt32>()));
		block.getByPosition(result).column = res_array;
	}
	else
	{
		throw Exception("Illegal column " + block.getByPosition(arguments[0]).column->getName()
				+ " of first argument of function " + getName(),
			ErrorCodes::ILLEGAL_COLUMN);
	}
}



String FunctionArrayUniq::getName() const
{
	return name;
}


DataTypePtr FunctionArrayUniq:: getReturnType(const DataTypes & arguments) const
{
	if (arguments.size() == 0)
		throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
			+ toString(arguments.size()) + ", should be at least 1.",
			ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

	for (size_t i = 0; i < arguments.size(); ++i)
	{
		const DataTypeArray * array_type = typeid_cast<const DataTypeArray *>(arguments[i].get());
		if (!array_type)
			throw Exception("All arguments for function " + getName() + " must be arrays but argument " +
				toString(i + 1) + " has type " + arguments[i]->getName() + ".", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
	}

	return std::make_shared<DataTypeUInt32>();
}


void FunctionArrayUniq::execute(Block & block, const ColumnNumbers & arguments, size_t result)
{
	if (arguments.size() == 1 && executeConst(block, arguments, result))
		return;

	Columns array_columns(arguments.size());
	const ColumnArray::Offsets_t * offsets = nullptr;
	ConstColumnPlainPtrs data_columns(arguments.size());

	for (size_t i = 0; i < arguments.size(); ++i)
	{
		ColumnPtr array_ptr = block.getByPosition(arguments[i]).column;
		const ColumnArray * array = typeid_cast<const ColumnArray *>(array_ptr.get());
		if (!array)
		{
			const ColumnConstArray * const_array = typeid_cast<const ColumnConstArray *>(
				block.getByPosition(arguments[i]).column.get());
			if (!const_array)
				throw Exception("Illegal column " + block.getByPosition(arguments[i]).column->getName()
					+ " of " + toString(i + 1) + "-th argument of function " + getName(),
					ErrorCodes::ILLEGAL_COLUMN);
			array_ptr = const_array->convertToFullColumn();
			array = typeid_cast<const ColumnArray *>(array_ptr.get());
		}
		array_columns[i] = array_ptr;
		const ColumnArray::Offsets_t & offsets_i = array->getOffsets();
		if (!i)
			offsets = &offsets_i;
		else if (offsets_i != *offsets)
			throw Exception("Lengths of all arrays passsed to " + getName() + " must be equal.",
				ErrorCodes::SIZES_OF_ARRAYS_DOESNT_MATCH);
		data_columns[i] = &array->getData();
	}

	const ColumnArray * first_array = typeid_cast<const ColumnArray *>(array_columns[0].get());
	auto res = std::make_shared<ColumnUInt32>();
	block.getByPosition(result).column = res;

	ColumnUInt32::Container_t & res_values = res->getData();
	res_values.resize(offsets->size());

	if (arguments.size() == 1)
	{
		if (!(	executeNumber<UInt8>	(first_array, res_values)
			||	executeNumber<UInt16>	(first_array, res_values)
			||	executeNumber<UInt32>	(first_array, res_values)
			||	executeNumber<UInt64>	(first_array, res_values)
			||	executeNumber<Int8>		(first_array, res_values)
			||	executeNumber<Int16>	(first_array, res_values)
			||	executeNumber<Int32>	(first_array, res_values)
			||	executeNumber<Int64>	(first_array, res_values)
			||	executeNumber<Float32>	(first_array, res_values)
			||	executeNumber<Float64>	(first_array, res_values)
			||	executeString			(first_array, res_values)))
			throw Exception("Illegal column " + block.getByPosition(arguments[0]).column->getName()
					+ " of first argument of function " + getName(),
				ErrorCodes::ILLEGAL_COLUMN);
	}
	else
	{
		if (!execute128bit(*offsets, data_columns, res_values))
			executeHashed(*offsets, data_columns, res_values);
	}
}


template <typename T>
bool FunctionArrayUniq::executeNumber(const ColumnArray * array, ColumnUInt32::Container_t & res_values)
{
	const ColumnVector<T> * nested = typeid_cast<const ColumnVector<T> *>(&array->getData());
	if (!nested)
		return false;
	const ColumnArray::Offsets_t & offsets = array->getOffsets();
	const typename ColumnVector<T>::Container_t & values = nested->getData();

	typedef ClearableHashSet<T, DefaultHash<T>, HashTableGrower<INITIAL_SIZE_DEGREE>,
		HashTableAllocatorWithStackMemory<(1 << INITIAL_SIZE_DEGREE) * sizeof(T)> > Set;

	Set set;
	size_t prev_off = 0;
	for (size_t i = 0; i < offsets.size(); ++i)
	{
		set.clear();
		size_t off = offsets[i];
		for (size_t j = prev_off; j < off; ++j)
			set.insert(values[j]);

		res_values[i] = set.size();
		prev_off = off;
	}
	return true;
}


bool FunctionArrayUniq::executeString(const ColumnArray * array, ColumnUInt32::Container_t & res_values)
{
	const ColumnString * nested = typeid_cast<const ColumnString *>(&array->getData());
	if (!nested)
		return false;
	const ColumnArray::Offsets_t & offsets = array->getOffsets();

	typedef ClearableHashSet<StringRef, StringRefHash, HashTableGrower<INITIAL_SIZE_DEGREE>,
		HashTableAllocatorWithStackMemory<(1 << INITIAL_SIZE_DEGREE) * sizeof(StringRef)> > Set;

	Set set;
	size_t prev_off = 0;
	for (size_t i = 0; i < offsets.size(); ++i)
	{
		set.clear();
		size_t off = offsets[i];
		for (size_t j = prev_off; j < off; ++j)
			set.insert(nested->getDataAt(j));

		res_values[i] = set.size();
		prev_off = off;
	}
	return true;
}


bool FunctionArrayUniq::executeConst(Block & block, const ColumnNumbers & arguments, size_t result)
{
	const ColumnConstArray * array = typeid_cast<const ColumnConstArray *>(block.getByPosition(arguments[0]).column.get());
	if (!array)
		return false;
	const Array & values = array->getData();

	std::set<Field> set;
	for (size_t i = 0; i < values.size(); ++i)
		set.insert(values[i]);

	block.getByPosition(result).column = std::make_shared<ColumnConstUInt32>(array->size(), set.size());
	return true;
}


bool FunctionArrayUniq::execute128bit(
	const ColumnArray::Offsets_t & offsets,
	const ConstColumnPlainPtrs & columns,
	ColumnUInt32::Container_t & res_values)
{
	size_t count = columns.size();
	size_t keys_bytes = 0;
	Sizes key_sizes(count);
	for (size_t j = 0; j < count; ++j)
	{
		if (!columns[j]->isFixed())
			return false;
		key_sizes[j] = columns[j]->sizeOfField();
		keys_bytes += key_sizes[j];
	}
	if (keys_bytes > 16)
		return false;

	typedef ClearableHashSet<UInt128, UInt128HashCRC32, HashTableGrower<INITIAL_SIZE_DEGREE>,
		HashTableAllocatorWithStackMemory<(1 << INITIAL_SIZE_DEGREE) * sizeof(UInt128)> > Set;

	Set set;
	size_t prev_off = 0;
	for (size_t i = 0; i < offsets.size(); ++i)
	{
		set.clear();
		size_t off = offsets[i];
		for (size_t j = prev_off; j < off; ++j)
			set.insert(packFixed<UInt128>(j, count, columns, key_sizes));

		res_values[i] = set.size();
		prev_off = off;
	}

	return true;
}


void FunctionArrayUniq::executeHashed(
	const ColumnArray::Offsets_t & offsets,
	const ConstColumnPlainPtrs & columns,
	ColumnUInt32::Container_t & res_values)
{
	size_t count = columns.size();

	typedef ClearableHashSet<UInt128, UInt128TrivialHash, HashTableGrower<INITIAL_SIZE_DEGREE>,
		HashTableAllocatorWithStackMemory<(1 << INITIAL_SIZE_DEGREE) * sizeof(UInt128)> > Set;

	Set set;
	size_t prev_off = 0;
	for (size_t i = 0; i < offsets.size(); ++i)
	{
		set.clear();
		size_t off = offsets[i];
		for (size_t j = prev_off; j < off; ++j)
			set.insert(hash128(j, count, columns));

		res_values[i] = set.size();
		prev_off = off;
	}
}



String FunctionArrayEnumerateUniq::getName() const
{
	return name;
}


DataTypePtr FunctionArrayEnumerateUniq::getReturnType(const DataTypes & arguments) const
{
	if (arguments.size() == 0)
		throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
			+ toString(arguments.size()) + ", should be at least 1.",
			ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

	for (size_t i = 0; i < arguments.size(); ++i)
	{
		const DataTypeArray * array_type = typeid_cast<const DataTypeArray *>(arguments[i].get());
		if (!array_type)
			throw Exception("All arguments for function " + getName() + " must be arrays but argument " +
				toString(i + 1) + " has type " + arguments[i]->getName() + ".", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
	}

	return std::make_shared<DataTypeArray>(std::make_shared<DataTypeUInt32>());
}


void FunctionArrayEnumerateUniq::execute(Block & block, const ColumnNumbers & arguments, size_t result)
{
	if (arguments.size() == 1 && executeConst(block, arguments, result))
		return;

	Columns array_columns(arguments.size());
	const ColumnArray::Offsets_t * offsets = nullptr;
	ConstColumnPlainPtrs data_columns(arguments.size());

	for (size_t i = 0; i < arguments.size(); ++i)
	{
		ColumnPtr array_ptr = block.getByPosition(arguments[i]).column;
		const ColumnArray * array = typeid_cast<const ColumnArray *>(array_ptr.get());
		if (!array)
		{
			const ColumnConstArray * const_array = typeid_cast<const ColumnConstArray *>(
				block.getByPosition(arguments[i]).column.get());
			if (!const_array)
				throw Exception("Illegal column " + block.getByPosition(arguments[i]).column->getName()
					+ " of " + toString(i + 1) + "-th argument of function " + getName(),
					ErrorCodes::ILLEGAL_COLUMN);
			array_ptr = const_array->convertToFullColumn();
			array = typeid_cast<const ColumnArray *>(array_ptr.get());
		}
		array_columns[i] = array_ptr;
		const ColumnArray::Offsets_t & offsets_i = array->getOffsets();
		if (!i)
			offsets = &offsets_i;
		else if (offsets_i != *offsets)
			throw Exception("Lengths of all arrays passsed to " + getName() + " must be equal.",
				ErrorCodes::SIZES_OF_ARRAYS_DOESNT_MATCH);
		data_columns[i] = &array->getData();
	}

	const ColumnArray * first_array = typeid_cast<const ColumnArray *>(array_columns[0].get());
	auto res_nested = std::make_shared<ColumnUInt32>();
	auto res_array = std::make_shared<ColumnArray>(res_nested, first_array->getOffsetsColumn());
	block.getByPosition(result).column = res_array;

	ColumnUInt32::Container_t & res_values = res_nested->getData();
	if (!offsets->empty())
		res_values.resize(offsets->back());

	if (arguments.size() == 1)
	{
		if (!(	executeNumber<UInt8>	(first_array, res_values)
			||	executeNumber<UInt16>	(first_array, res_values)
			||	executeNumber<UInt32>	(first_array, res_values)
			||	executeNumber<UInt64>	(first_array, res_values)
			||	executeNumber<Int8>		(first_array, res_values)
			||	executeNumber<Int16>	(first_array, res_values)
			||	executeNumber<Int32>	(first_array, res_values)
			||	executeNumber<Int64>	(first_array, res_values)
			||	executeNumber<Float32>	(first_array, res_values)
			||	executeNumber<Float64>	(first_array, res_values)
			||	executeString			(first_array, res_values)))
			throw Exception("Illegal column " + block.getByPosition(arguments[0]).column->getName()
					+ " of first argument of function " + getName(),
				ErrorCodes::ILLEGAL_COLUMN);
	}
	else
	{
		if (!execute128bit(*offsets, data_columns, res_values))
			executeHashed(*offsets, data_columns, res_values);
	}
}


template <typename T>
bool FunctionArrayEnumerateUniq::executeNumber(const ColumnArray * array, ColumnUInt32::Container_t & res_values)
{
	const ColumnVector<T> * nested = typeid_cast<const ColumnVector<T> *>(&array->getData());
	if (!nested)
		return false;
	const ColumnArray::Offsets_t & offsets = array->getOffsets();
	const typename ColumnVector<T>::Container_t & values = nested->getData();

	typedef ClearableHashMap<T, UInt32, DefaultHash<T>, HashTableGrower<INITIAL_SIZE_DEGREE>,
		HashTableAllocatorWithStackMemory<(1 << INITIAL_SIZE_DEGREE) * sizeof(T)> > ValuesToIndices;

	ValuesToIndices indices;
	size_t prev_off = 0;
	for (size_t i = 0; i < offsets.size(); ++i)
	{
		indices.clear();
		size_t off = offsets[i];
		for (size_t j = prev_off; j < off; ++j)
		{
			res_values[j] = ++indices[values[j]];
		}
		prev_off = off;
	}
	return true;
}


bool FunctionArrayEnumerateUniq::executeString(const ColumnArray * array, ColumnUInt32::Container_t & res_values)
{
	const ColumnString * nested = typeid_cast<const ColumnString *>(&array->getData());
	if (!nested)
		return false;
	const ColumnArray::Offsets_t & offsets = array->getOffsets();

	size_t prev_off = 0;
	typedef ClearableHashMap<StringRef, UInt32, StringRefHash, HashTableGrower<INITIAL_SIZE_DEGREE>,
		HashTableAllocatorWithStackMemory<(1 << INITIAL_SIZE_DEGREE) * sizeof(StringRef)> > ValuesToIndices;

	ValuesToIndices indices;
	for (size_t i = 0; i < offsets.size(); ++i)
	{
		indices.clear();
		size_t off = offsets[i];
		for (size_t j = prev_off; j < off; ++j)
		{
			res_values[j] = ++indices[nested->getDataAt(j)];
		}
		prev_off = off;
	}
	return true;
}


bool FunctionArrayEnumerateUniq::executeConst(Block & block, const ColumnNumbers & arguments, size_t result)
{
	const ColumnConstArray * array = typeid_cast<const ColumnConstArray *>(block.getByPosition(arguments[0]).column.get());
	if (!array)
		return false;
	const Array & values = array->getData();

	Array res_values(values.size());
	std::map<Field, UInt32> indices;
	for (size_t i = 0; i < values.size(); ++i)
	{
		res_values[i] = static_cast<UInt64>(++indices[values[i]]);
	}

	auto res_array = std::make_shared<ColumnConstArray>(array->size(), res_values, std::make_shared<DataTypeArray>(std::make_shared<DataTypeUInt32>()));
	block.getByPosition(result).column = res_array;

	return true;
}


bool FunctionArrayEnumerateUniq::execute128bit(
	const ColumnArray::Offsets_t & offsets,
	const ConstColumnPlainPtrs & columns,
	ColumnUInt32::Container_t & res_values)
{
	size_t count = columns.size();
	size_t keys_bytes = 0;
	Sizes key_sizes(count);
	for (size_t j = 0; j < count; ++j)
	{
		if (!columns[j]->isFixed())
			return false;
		key_sizes[j] = columns[j]->sizeOfField();
		keys_bytes += key_sizes[j];
	}
	if (keys_bytes > 16)
		return false;

	typedef ClearableHashMap<UInt128, UInt32, UInt128HashCRC32, HashTableGrower<INITIAL_SIZE_DEGREE>,
		HashTableAllocatorWithStackMemory<(1 << INITIAL_SIZE_DEGREE) * sizeof(UInt128)> > ValuesToIndices;

	ValuesToIndices indices;
	size_t prev_off = 0;
	for (size_t i = 0; i < offsets.size(); ++i)
	{
		indices.clear();
		size_t off = offsets[i];
		for (size_t j = prev_off; j < off; ++j)
		{
			res_values[j] = ++indices[packFixed<UInt128>(j, count, columns, key_sizes)];
		}
		prev_off = off;
	}

	return true;
}


void FunctionArrayEnumerateUniq::executeHashed(
	const ColumnArray::Offsets_t & offsets,
	const ConstColumnPlainPtrs & columns,
	ColumnUInt32::Container_t & res_values)
{
	size_t count = columns.size();

	typedef ClearableHashMap<UInt128, UInt32, UInt128TrivialHash, HashTableGrower<INITIAL_SIZE_DEGREE>,
		HashTableAllocatorWithStackMemory<(1 << INITIAL_SIZE_DEGREE) * sizeof(UInt128)> > ValuesToIndices;

	ValuesToIndices indices;
	size_t prev_off = 0;
	for (size_t i = 0; i < offsets.size(); ++i)
	{
		indices.clear();
		size_t off = offsets[i];
		for (size_t j = prev_off; j < off; ++j)
		{
			res_values[j] = ++indices[hash128(j, count, columns)];
		}
		prev_off = off;
	}
}



String FunctionRange::getName() const
{
	return name;
}


DataTypePtr FunctionRange::getReturnType(const DataTypes & arguments) const
{
	if (arguments.size() != 1)
		throw Exception{
			"Number of arguments for function " + getName() + " doesn't match: passed "
			+ toString(arguments.size()) + ", should be 1.",
			ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH
		};

	const auto arg = arguments.front().get();

	if (!typeid_cast<const DataTypeUInt8 *>(arg) &&
		!typeid_cast<const DataTypeUInt16 *>(arg) &&
		!typeid_cast<const DataTypeUInt32 *>(arg) &
		!typeid_cast<const DataTypeUInt64 *>(arg))
	{
		throw Exception{
			"Illegal type " + arg->getName() + " of argument of function " + getName(),
			ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT
		};
	}

	return std::make_shared<DataTypeArray>(arg->clone());
}


template <typename T>
bool FunctionRange::execute(Block & block, const IColumn * const arg, const size_t result)
{
	if (const auto in = typeid_cast<const ColumnVector<T> *>(arg))
	{
		const auto & in_data = in->getData();
		const auto total_values = std::accumulate(std::begin(in_data), std::end(in_data), std::size_t{},
			[this] (const std::size_t lhs, const std::size_t rhs) {
				const auto sum = lhs + rhs;
				if (sum < lhs)
					throw Exception{
						"A call to function " + getName() + " overflows, investigate the values of arguments you are passing",
						ErrorCodes::ARGUMENT_OUT_OF_BOUND
					};

				return sum;
			});

		if (total_values > max_elements)
			throw Exception{
				"A call to function " + getName() + " would produce " + std::to_string(total_values) +
					" array elements, which is greater than the allowed maximum of " + std::to_string(max_elements),
				ErrorCodes::ARGUMENT_OUT_OF_BOUND
			};

		const auto data_col = std::make_shared<ColumnVector<T>>(total_values);
		const auto out = std::make_shared<ColumnArray>(
			data_col,
			std::make_shared<ColumnArray::ColumnOffsets_t>(in->size()));
		block.getByPosition(result).column = out;

		auto & out_data = data_col->getData();
		auto & out_offsets = out->getOffsets();

		IColumn::Offset_t offset{};
		for (const auto i : ext::range(0, in->size()))
		{
			std::copy(ext::make_range_iterator(T{}), ext::make_range_iterator(in_data[i]), &out_data[offset]);
			offset += in_data[i];
			out_offsets[i] = offset;
		}

		return true;
	}
	else if (const auto in = typeid_cast<const ColumnConst<T> *>(arg))
	{
		const auto & in_data = in->getData();
		if ((in_data != 0) && (in->size() > (std::numeric_limits<std::size_t>::max() / in_data)))
			throw Exception{
				"A call to function " + getName() + " overflows, investigate the values of arguments you are passing",
				ErrorCodes::ARGUMENT_OUT_OF_BOUND
			};

		const std::size_t total_values = in->size() * in_data;
		if (total_values > max_elements)
			throw Exception{
				"A call to function " + getName() + " would produce " + std::to_string(total_values) +
					" array elements, which is greater than the allowed maximum of " + std::to_string(max_elements),
				ErrorCodes::ARGUMENT_OUT_OF_BOUND
			};

		const auto data_col = std::make_shared<ColumnVector<T>>(total_values);
		const auto out = std::make_shared<ColumnArray>(
			data_col,
			std::make_shared<ColumnArray::ColumnOffsets_t>(in->size()));
		block.getByPosition(result).column = out;

		auto & out_data = data_col->getData();
		auto & out_offsets = out->getOffsets();

		IColumn::Offset_t offset{};
		for (const auto i : ext::range(0, in->size()))
		{
			std::copy(ext::make_range_iterator(T{}), ext::make_range_iterator(in_data), &out_data[offset]);
			offset += in_data;
			out_offsets[i] = offset;
		}

		return true;
	}

	return false;
}


void FunctionRange::execute(Block & block, const ColumnNumbers & arguments, const size_t result)
{
	const auto col = block.getByPosition(arguments[0]).column.get();

	if (!execute<UInt8>(block, col, result) &&
		!execute<UInt16>(block, col, result) &&
		!execute<UInt32>(block, col, result) &&
		!execute<UInt64>(block, col, result))
	{
		throw Exception{
			"Illegal column " + col->getName() + " of argument of function " + getName(),
			ErrorCodes::ILLEGAL_COLUMN
		};
	}
}



String FunctionEmptyArrayToSingle::getName() const
{
	return name;
}


DataTypePtr FunctionEmptyArrayToSingle::getReturnType(const DataTypes & arguments) const
{
	if (arguments.size() != 1)
		throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
			+ toString(arguments.size()) + ", should be 1.",
			ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

	const DataTypeArray * array_type = typeid_cast<const DataTypeArray *>(arguments[0].get());
	if (!array_type)
		throw Exception("Argument for function " + getName() + " must be array.",
			ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

	return arguments[0]->clone();
}


void FunctionEmptyArrayToSingle::execute(Block & block, const ColumnNumbers & arguments, size_t result)
{
	if (executeConst(block, arguments, result))
		return;

	const ColumnArray * array = typeid_cast<const ColumnArray *>(block.getByPosition(arguments[0]).column.get());
	if (!array)
		throw Exception("Illegal column " + block.getByPosition(arguments[0]).column->getName() + " of first argument of function " + getName(),
			ErrorCodes::ILLEGAL_COLUMN);

	ColumnPtr res_ptr = array->cloneEmpty();
	block.getByPosition(result).column = res_ptr;
	ColumnArray & res = static_cast<ColumnArray &>(*res_ptr);

	const IColumn & src_data = array->getData();
	const ColumnArray::Offsets_t & src_offsets = array->getOffsets();
	IColumn & res_data = res.getData();
	ColumnArray::Offsets_t & res_offsets = res.getOffsets();

	if (!(	executeNumber<UInt8>	(src_data, src_offsets, res_data, res_offsets)
		||	executeNumber<UInt16>	(src_data, src_offsets, res_data, res_offsets)
		||	executeNumber<UInt32>	(src_data, src_offsets, res_data, res_offsets)
		||	executeNumber<UInt64>	(src_data, src_offsets, res_data, res_offsets)
		||	executeNumber<Int8>		(src_data, src_offsets, res_data, res_offsets)
		||	executeNumber<Int16>	(src_data, src_offsets, res_data, res_offsets)
		||	executeNumber<Int32>	(src_data, src_offsets, res_data, res_offsets)
		||	executeNumber<Int64>	(src_data, src_offsets, res_data, res_offsets)
		||	executeNumber<Float32>	(src_data, src_offsets, res_data, res_offsets)
		||	executeNumber<Float64>	(src_data, src_offsets, res_data, res_offsets)
		||	executeString			(src_data, src_offsets, res_data, res_offsets)
		||	executeFixedString		(src_data, src_offsets, res_data, res_offsets)))
		throw Exception("Illegal column " + block.getByPosition(arguments[0]).column->getName()
			+ " of first argument of function " + getName(),
			ErrorCodes::ILLEGAL_COLUMN);
}


bool FunctionEmptyArrayToSingle::executeConst(Block & block, const ColumnNumbers & arguments, size_t result)
{
	if (const ColumnConstArray * const_array = typeid_cast<const ColumnConstArray *>(block.getByPosition(arguments[0]).column.get()))
	{
		if (const_array->getData().empty())
		{
			auto nested_type = typeid_cast<const DataTypeArray &>(*block.getByPosition(arguments[0]).type).getNestedType();

			block.getByPosition(result).column = std::make_shared<ColumnConstArray>(
				block.rowsInFirstColumn(),
				Array{nested_type->getDefault()},
				nested_type->clone());
		}
		else
			block.getByPosition(result).column = block.getByPosition(arguments[0]).column;

		return true;
	}
	else
		return false;
}


template <typename T>
bool FunctionEmptyArrayToSingle::executeNumber(
	const IColumn & src_data, const ColumnArray::Offsets_t & src_offsets,
	IColumn & res_data_col, ColumnArray::Offsets_t & res_offsets)
{
	if (const ColumnVector<T> * src_data_concrete = typeid_cast<const ColumnVector<T> *>(&src_data))
	{
		const PaddedPODArray<T> & src_data = src_data_concrete->getData();
		PaddedPODArray<T> & res_data = typeid_cast<ColumnVector<T> &>(res_data_col).getData();
		size_t size = src_offsets.size();
		res_offsets.resize(size);
		res_data.reserve(src_data.size());

		ColumnArray::Offset_t src_prev_offset = 0;
		ColumnArray::Offset_t res_prev_offset = 0;

		for (size_t i = 0; i < size; ++i)
		{
			if (src_offsets[i] != src_prev_offset)
			{
				size_t size_to_write = src_offsets[i] - src_prev_offset;
				size_t prev_res_data_size = res_data.size();
				res_data.resize(prev_res_data_size + size_to_write);
				memcpy(&res_data[prev_res_data_size], &src_data[src_prev_offset], size_to_write * sizeof(T));
				res_prev_offset += size_to_write;
				res_offsets[i] = res_prev_offset;
			}
			else
			{
				res_data.push_back(T());
				++res_prev_offset;
				res_offsets[i] = res_prev_offset;
			}

			src_prev_offset = src_offsets[i];
		}

		return true;
	}
	else
		return false;
}


bool FunctionEmptyArrayToSingle::executeFixedString(
	const IColumn & src_data, const ColumnArray::Offsets_t & src_offsets,
	IColumn & res_data_col, ColumnArray::Offsets_t & res_offsets)
{
	if (const ColumnFixedString * src_data_concrete = typeid_cast<const ColumnFixedString *>(&src_data))
	{
		const size_t n = src_data_concrete->getN();
		const ColumnFixedString::Chars_t & src_data = src_data_concrete->getChars();
		ColumnFixedString::Chars_t & res_data = typeid_cast<ColumnFixedString &>(res_data_col).getChars();
		size_t size = src_offsets.size();
		res_offsets.resize(size);
		res_data.reserve(src_data.size());

		ColumnArray::Offset_t src_prev_offset = 0;
		ColumnArray::Offset_t res_prev_offset = 0;

		for (size_t i = 0; i < size; ++i)
		{
			if (src_offsets[i] != src_prev_offset)
			{
				size_t size_to_write = src_offsets[i] - src_prev_offset;
				size_t prev_res_data_size = res_data.size();
				res_data.resize(prev_res_data_size + size_to_write * n);
				memcpy(&res_data[prev_res_data_size], &src_data[src_prev_offset], size_to_write * n);
				res_prev_offset += size_to_write;
				res_offsets[i] = res_prev_offset;
			}
			else
			{
				size_t prev_res_data_size = res_data.size();
				res_data.resize(prev_res_data_size + n);
				memset(&res_data[prev_res_data_size], 0, n);
				++res_prev_offset;
				res_offsets[i] = res_prev_offset;
			}

			src_prev_offset = src_offsets[i];
		}

		return true;
	}
	else
		return false;
}


bool FunctionEmptyArrayToSingle::executeString(
	const IColumn & src_data, const ColumnArray::Offsets_t & src_array_offsets,
	IColumn & res_data_col, ColumnArray::Offsets_t & res_array_offsets)
{
	if (const ColumnString * src_data_concrete = typeid_cast<const ColumnString *>(&src_data))
	{
		const ColumnString::Offsets_t & src_string_offsets = src_data_concrete->getOffsets();
		ColumnString::Offsets_t & res_string_offsets = typeid_cast<ColumnString &>(res_data_col).getOffsets();

		const ColumnString::Chars_t & src_data = src_data_concrete->getChars();
		ColumnString::Chars_t & res_data = typeid_cast<ColumnString &>(res_data_col).getChars();

		size_t size = src_array_offsets.size();
		res_array_offsets.resize(size);
		res_string_offsets.reserve(src_string_offsets.size());
		res_data.reserve(src_data.size());

		ColumnArray::Offset_t src_array_prev_offset = 0;
		ColumnArray::Offset_t res_array_prev_offset = 0;

		ColumnString::Offset_t src_string_prev_offset = 0;
		ColumnString::Offset_t res_string_prev_offset = 0;

		for (size_t i = 0; i < size; ++i)
		{
			if (src_array_offsets[i] != src_array_prev_offset)
			{
				size_t array_size = src_array_offsets[i] - src_array_prev_offset;

				size_t bytes_to_copy = 0;
				size_t from_string_prev_offset_local = src_string_prev_offset;
				for (size_t j = 0; j < array_size; ++j)
				{
					size_t string_size = src_string_offsets[src_array_prev_offset + j] - from_string_prev_offset_local;

					res_string_prev_offset += string_size;
					res_string_offsets.push_back(res_string_prev_offset);

					from_string_prev_offset_local += string_size;
					bytes_to_copy += string_size;
				}

				size_t res_data_old_size = res_data.size();
				res_data.resize(res_data_old_size + bytes_to_copy);
				memcpy(&res_data[res_data_old_size], &src_data[src_string_prev_offset], bytes_to_copy);

				res_array_prev_offset += array_size;
				res_array_offsets[i] = res_array_prev_offset;
			}
			else
			{
				res_data.push_back(0);	/// Пустая строка, включая ноль на конце.

				++res_string_prev_offset;
				res_string_offsets.push_back(res_string_prev_offset);

				++res_array_prev_offset;
				res_array_offsets[i] = res_array_prev_offset;
			}

			src_array_prev_offset = src_array_offsets[i];

			if (src_array_prev_offset)
				src_string_prev_offset = src_string_offsets[src_array_prev_offset - 1];
		}

		return true;
	}
	else
		return false;
}



String FunctionArrayReverse::getName() const
{
	return name;
}


DataTypePtr FunctionArrayReverse::getReturnType(const DataTypes & arguments) const
{
	if (arguments.size() != 1)
		throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
			+ toString(arguments.size()) + ", should be 1.",
			ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

	const DataTypeArray * array_type = typeid_cast<const DataTypeArray *>(arguments[0].get());
	if (!array_type)
		throw Exception("Argument for function " + getName() + " must be array.",
			ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

	return arguments[0]->clone();
}


void FunctionArrayReverse::execute(Block & block, const ColumnNumbers & arguments, size_t result)
{
	if (executeConst(block, arguments, result))
		return;

	const ColumnArray * array = typeid_cast<const ColumnArray *>(block.getByPosition(arguments[0]).column.get());
	if (!array)
		throw Exception("Illegal column " + block.getByPosition(arguments[0]).column->getName() + " of first argument of function " + getName(),
			ErrorCodes::ILLEGAL_COLUMN);

	ColumnPtr res_ptr = array->cloneEmpty();
	block.getByPosition(result).column = res_ptr;
	ColumnArray & res = static_cast<ColumnArray &>(*res_ptr);

	const IColumn & src_data = array->getData();
	const ColumnArray::Offsets_t & offsets = array->getOffsets();
	IColumn & res_data = res.getData();
	res.getOffsetsColumn() = array->getOffsetsColumn();

	if (!(	executeNumber<UInt8>	(src_data, offsets, res_data)
		||	executeNumber<UInt16>	(src_data, offsets, res_data)
		||	executeNumber<UInt32>	(src_data, offsets, res_data)
		||	executeNumber<UInt64>	(src_data, offsets, res_data)
		||	executeNumber<Int8>		(src_data, offsets, res_data)
		||	executeNumber<Int16>	(src_data, offsets, res_data)
		||	executeNumber<Int32>	(src_data, offsets, res_data)
		||	executeNumber<Int64>	(src_data, offsets, res_data)
		||	executeNumber<Float32>	(src_data, offsets, res_data)
		||	executeNumber<Float64>	(src_data, offsets, res_data)
		||	executeString			(src_data, offsets, res_data)
		||	executeFixedString		(src_data, offsets, res_data)))
		throw Exception("Illegal column " + block.getByPosition(arguments[0]).column->getName()
			+ " of first argument of function " + getName(),
			ErrorCodes::ILLEGAL_COLUMN);
}


bool FunctionArrayReverse::executeConst(Block & block, const ColumnNumbers & arguments, size_t result)
{
	if (const ColumnConstArray * const_array = typeid_cast<const ColumnConstArray *>(block.getByPosition(arguments[0]).column.get()))
	{
		const Array & arr = const_array->getData();

		size_t size = arr.size();
		Array res(size);

		for (size_t i = 0; i < size; ++i)
			res[i] = arr[size - i - 1];

		block.getByPosition(result).column = std::make_shared<ColumnConstArray>(
			block.rowsInFirstColumn(),
			res,
			block.getByPosition(arguments[0]).type->clone());

		return true;
	}
	else
		return false;
}


template <typename T>
bool FunctionArrayReverse::executeNumber(
	const IColumn & src_data, const ColumnArray::Offsets_t & src_offsets,
	IColumn & res_data_col)
{
	if (const ColumnVector<T> * src_data_concrete = typeid_cast<const ColumnVector<T> *>(&src_data))
	{
		const PaddedPODArray<T> & src_data = src_data_concrete->getData();
		PaddedPODArray<T> & res_data = typeid_cast<ColumnVector<T> &>(res_data_col).getData();
		size_t size = src_offsets.size();
		res_data.resize(src_data.size());

		ColumnArray::Offset_t src_prev_offset = 0;

		for (size_t i = 0; i < size; ++i)
		{
			const T * src = &src_data[src_prev_offset];
			const T * src_end = &src_data[src_offsets[i]];

			if (src == src_end)
				continue;

			T * dst = &res_data[src_offsets[i] - 1];

			while (src < src_end)
			{
				*dst = *src;
				++src;
				--dst;
			}

			src_prev_offset = src_offsets[i];
		}

		return true;
	}
	else
		return false;
}


bool FunctionArrayReverse::executeFixedString(
	const IColumn & src_data, const ColumnArray::Offsets_t & src_offsets,
	IColumn & res_data_col)
{
	if (const ColumnFixedString * src_data_concrete = typeid_cast<const ColumnFixedString *>(&src_data))
	{
		const size_t n = src_data_concrete->getN();
		const ColumnFixedString::Chars_t & src_data = src_data_concrete->getChars();
		ColumnFixedString::Chars_t & res_data = typeid_cast<ColumnFixedString &>(res_data_col).getChars();
		size_t size = src_offsets.size();
		res_data.resize(src_data.size());

		ColumnArray::Offset_t src_prev_offset = 0;

		for (size_t i = 0; i < size; ++i)
		{
			const UInt8 * src = &src_data[src_prev_offset * n];
			const UInt8 * src_end = &src_data[src_offsets[i] * n];

			if (src == src_end)
				continue;

			UInt8 * dst = &res_data[src_offsets[i] * n - n];

			while (src < src_end)
			{
				memcpySmallAllowReadWriteOverflow15(dst, src, n);
				src += n;
				dst -= n;
			}

			src_prev_offset = src_offsets[i];
		}

		return true;
	}
	else
		return false;
}


bool FunctionArrayReverse::executeString(
	const IColumn & src_data, const ColumnArray::Offsets_t & src_array_offsets,
	IColumn & res_data_col)
{
	if (const ColumnString * src_data_concrete = typeid_cast<const ColumnString *>(&src_data))
	{
		const ColumnString::Offsets_t & src_string_offsets = src_data_concrete->getOffsets();
		ColumnString::Offsets_t & res_string_offsets = typeid_cast<ColumnString &>(res_data_col).getOffsets();

		const ColumnString::Chars_t & src_data = src_data_concrete->getChars();
		ColumnString::Chars_t & res_data = typeid_cast<ColumnString &>(res_data_col).getChars();

		size_t size = src_array_offsets.size();
		res_string_offsets.resize(src_string_offsets.size());
		res_data.resize(src_data.size());

		ColumnArray::Offset_t src_array_prev_offset = 0;
		ColumnString::Offset_t res_string_prev_offset = 0;

		for (size_t i = 0; i < size; ++i)
		{
			if (src_array_offsets[i] != src_array_prev_offset)
			{
				size_t array_size = src_array_offsets[i] - src_array_prev_offset;

				for (size_t j = 0; j < array_size; ++j)
				{
					size_t j_reversed = array_size - j - 1;

					auto src_pos = src_array_prev_offset + j_reversed == 0 ? 0 : src_string_offsets[src_array_prev_offset + j_reversed - 1];
					size_t string_size = src_string_offsets[src_array_prev_offset + j_reversed] - src_pos;

					memcpySmallAllowReadWriteOverflow15(&res_data[res_string_prev_offset], &src_data[src_pos], string_size);

					res_string_prev_offset += string_size;
					res_string_offsets[src_array_prev_offset + j] = res_string_prev_offset;
				}
			}

			src_array_prev_offset = src_array_offsets[i];
		}

		return true;
	}
	else
		return false;
}



String FunctionArrayReduce::getName() const
{
	return name;
}


void FunctionArrayReduce::getReturnTypeAndPrerequisites(
	const ColumnsWithTypeAndName & arguments,
	DataTypePtr & out_return_type,
	std::vector<ExpressionAction> & out_prerequisites)
{
	/// Первый аргумент - константная строка с именем агрегатной функции (возможно, с параметрами в скобках, например: "quantile(0.99)").

	if (arguments.size() < 2)
		throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
			+ toString(arguments.size()) + ", should be at least 2.",
			ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

	const ColumnConstString * aggregate_function_name_column = typeid_cast<const ColumnConstString *>(arguments[0].column.get());
	if (!aggregate_function_name_column)
		throw Exception("First argument for function " + getName() + " must be constant string: name of aggregate function.",
			ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

	DataTypes argument_types(arguments.size() - 1);
	for (size_t i = 1, size = arguments.size(); i < size; ++i)
	{
		const DataTypeArray * arg = typeid_cast<const DataTypeArray *>(arguments[i].type.get());
		if (!arg)
			throw Exception("Argument " + toString(i) + " for function " + getName() + " must be an array but it has type "
				+ arguments[i].type->getName() + ".", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

		argument_types[i - 1] = arg->getNestedType()->clone();
	}

	if (!aggregate_function)
	{
		const String & aggregate_function_name_with_params = aggregate_function_name_column->getData();

		if (aggregate_function_name_with_params.empty())
			throw Exception("First argument for function " + getName() + " (name of aggregate function) cannot be empty.",
				ErrorCodes::BAD_ARGUMENTS);

		bool has_parameters = ')' == aggregate_function_name_with_params.back();

		String aggregate_function_name = aggregate_function_name_with_params;
		String parameters;
		Array params_row;

		if (has_parameters)
		{
			size_t pos = aggregate_function_name_with_params.find('(');
			if (pos == std::string::npos || pos + 2 >= aggregate_function_name_with_params.size())
				throw Exception("First argument for function " + getName() + " doesn't look like aggregate function name.",
					ErrorCodes::BAD_ARGUMENTS);

			aggregate_function_name = aggregate_function_name_with_params.substr(0, pos);
			parameters = aggregate_function_name_with_params.substr(pos + 1, aggregate_function_name_with_params.size() - pos - 2);

			if (aggregate_function_name.empty())
				throw Exception("First argument for function " + getName() + " doesn't look like aggregate function name.",
					ErrorCodes::BAD_ARGUMENTS);

			ParserExpressionList params_parser(false);
			ASTPtr args_ast = parseQuery(params_parser,
				parameters.data(), parameters.data() + parameters.size(),
				"parameters of aggregate function");

			ASTExpressionList & args_list = typeid_cast<ASTExpressionList &>(*args_ast);

			if (args_list.children.empty())
				throw Exception("Incorrect list of parameters to aggregate function "
					+ aggregate_function_name, ErrorCodes::BAD_ARGUMENTS);

			params_row.reserve(args_list.children.size());
			for (const auto & child : args_list.children)
			{
				const ASTLiteral * lit = typeid_cast<const ASTLiteral *>(child.get());
				if (!lit)
					throw Exception("Parameters to aggregate functions must be literals",
						ErrorCodes::PARAMETERS_TO_AGGREGATE_FUNCTIONS_MUST_BE_LITERALS);

				params_row.push_back(lit->value);
			}
		}

		aggregate_function = AggregateFunctionFactory().get(aggregate_function_name, argument_types);

		/// Потому что владение состояниями агрегатных функций никуда не отдаётся.
		if (aggregate_function->isState())
			throw Exception("Using aggregate function with -State modifier in function arrayReduce is not supported", ErrorCodes::BAD_ARGUMENTS);

		if (has_parameters)
			aggregate_function->setParameters(params_row);
		aggregate_function->setArguments(argument_types);
	}

	out_return_type = aggregate_function->getReturnType();
}


void FunctionArrayReduce::execute(Block & block, const ColumnNumbers & arguments, size_t result)
{
	IAggregateFunction & agg_func = *aggregate_function.get();
	std::unique_ptr<char[]> place_holder { new char[agg_func.sizeOfData()] };
	AggregateDataPtr place = place_holder.get();

	size_t rows = block.rowsInFirstColumn();

	/// Агрегатные функции не поддерживают константные столбцы. Поэтому, материализуем их.
	std::vector<ColumnPtr> materialized_columns;

	std::vector<const IColumn *> aggregate_arguments_vec(arguments.size() - 1);

	for (size_t i = 0, size = arguments.size() - 1; i < size; ++i)
	{
		const IColumn * col = block.unsafeGetByPosition(arguments[i + 1]).column.get();
		if (const ColumnArray * arr = typeid_cast<const ColumnArray *>(col))
		{
			aggregate_arguments_vec[i] = arr->getDataPtr().get();
		}
		else if (const ColumnConstArray * arr = typeid_cast<const ColumnConstArray *>(col))
		{
			materialized_columns.emplace_back(arr->convertToFullColumn());
			aggregate_arguments_vec[i] = typeid_cast<const ColumnArray &>(*materialized_columns.back().get()).getDataPtr().get();
		}
		else
			throw Exception("Illegal column " + col->getName() + " as argument of function " + getName(), ErrorCodes::ILLEGAL_COLUMN);

	}
	const IColumn ** aggregate_arguments = aggregate_arguments_vec.data();

	const ColumnArray::Offsets_t & offsets = typeid_cast<const ColumnArray &>(!materialized_columns.empty()
		? *materialized_columns.front().get()
		: *block.unsafeGetByPosition(arguments[1]).column.get()).getOffsets();

	ColumnPtr result_holder = block.getByPosition(result).type->createColumn();
	block.getByPosition(result).column = result_holder;
	IColumn & res_col = *result_holder.get();

	ColumnArray::Offset_t current_offset = 0;
	for (size_t i = 0; i < rows; ++i)
	{
		agg_func.create(place);
		ColumnArray::Offset_t next_offset = offsets[i];

		try
		{
			for (size_t j = current_offset; j < next_offset; ++j)
				agg_func.add(place, aggregate_arguments, j, nullptr);

			agg_func.insertResultInto(place, res_col);
		}
		catch (...)
		{
			agg_func.destroy(place);
			throw;
		}

		agg_func.destroy(place);
		current_offset = next_offset;
	}
}


void registerFunctionsArray(FunctionFactory & factory)
{
	factory.registerFunction<FunctionArray>();
	factory.registerFunction<FunctionArrayElement>();
	factory.registerFunction<FunctionHas>();
	factory.registerFunction<FunctionIndexOf>();
	factory.registerFunction<FunctionCountEqual>();
	factory.registerFunction<FunctionArrayEnumerate>();
	factory.registerFunction<FunctionArrayEnumerateUniq>();
	factory.registerFunction<FunctionArrayUniq>();
	factory.registerFunction<FunctionEmptyArrayUInt8>();
	factory.registerFunction<FunctionEmptyArrayUInt16>();
	factory.registerFunction<FunctionEmptyArrayUInt32>();
	factory.registerFunction<FunctionEmptyArrayUInt64>();
	factory.registerFunction<FunctionEmptyArrayInt8>();
	factory.registerFunction<FunctionEmptyArrayInt16>();
	factory.registerFunction<FunctionEmptyArrayInt32>();
	factory.registerFunction<FunctionEmptyArrayInt64>();
	factory.registerFunction<FunctionEmptyArrayFloat32>();
	factory.registerFunction<FunctionEmptyArrayFloat64>();
	factory.registerFunction<FunctionEmptyArrayDate>();
	factory.registerFunction<FunctionEmptyArrayDateTime>();
	factory.registerFunction<FunctionEmptyArrayString>();
	factory.registerFunction<FunctionEmptyArrayToSingle>();
	factory.registerFunction<FunctionRange>();
	factory.registerFunction<FunctionArrayReduce>();
}

}
