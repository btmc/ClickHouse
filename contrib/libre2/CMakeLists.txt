set (re2_headers
	./re2/tostring.cc
	./re2/dfa.cc
	./re2/prefilter.cc
	./re2/compile.cc
	./re2/regexp.cc
	./re2/onepass.cc
	./re2/prefilter_tree.cc
	./re2/set.cc
	./re2/filtered_re2.cc
	./re2/perl_groups.cc
	./re2/parse.cc
	./re2/nfa.cc
	./re2/bitstate.cc
	./re2/simplify.cc
	./re2/unicode_groups.cc
	./re2/mimics_pcre.cc
	./re2/re2.cc
	./re2/prog.cc
	./re2/unicode_casefold.cc
	./util/test.cc
	./util/strutil.cc
	./util/stringpiece.cc
	./util/hash.cc
	./util/arena.cc
	./util/benchmark.cc
	./util/valgrind.cc
	./util/pcre.cc
	./util/stringprintf.cc
	./util/rune.cc
	./util/random.cc
	./util/thread.cc
)

# Building re2 which is thread-safe and re2_st which is not.
# re2 changes its state during matching of regular expression, e.g. creates temporary DFA.
# It uses RWLock to process the same regular expression object from different threads.
# In order to avoid redundant locks in some cases, we use not thread-safe version of the library (re2_st).
add_definitions (-DNDEBUG)

add_library (re2 ${re2_headers})
add_library (re2_st ${re2_headers})

set_target_properties (re2_st PROPERTIES COMPILE_DEFINITIONS "NO_THREADS;re2=re2_st")

message ("Creating headers for re2_st library.")
file (MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/re2_st)
foreach (FILENAME filtered_re2.h re2.h set.h stringpiece.h variadic_function.h)
	file (READ ${CMAKE_CURRENT_SOURCE_DIR}/re2/${FILENAME} CONTENT)
	string (REGEX REPLACE "using re2::RE2;" "" CONTENT "${CONTENT}")
	string (REGEX REPLACE "namespace re2" "namespace re2_st" CONTENT "${CONTENT}")
	string (REGEX REPLACE "re2::" "re2_st::" CONTENT "${CONTENT}")
	string (REGEX REPLACE "\"re2/" "\"re2_st/" CONTENT "${CONTENT}")
	string (REGEX REPLACE "(.\\*?_H)" "\\1_ST" CONTENT "${CONTENT}")
	file (WRITE ${CMAKE_CURRENT_BINARY_DIR}/re2_st/${FILENAME} "${CONTENT}")
endforeach ()
