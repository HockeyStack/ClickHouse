use daachorse::{DoubleArrayAhoCorasick, DoubleArrayAhoCorasickBuilder, MatchKind};
use std::panic;
use std::slice;

/// Opaque handle to the Aho-Corasick automaton.
///
/// The automaton searches raw bytes only. Case folding is done by the C++ caller before patterns
/// and haystacks cross this boundary (see `MultiSearchAhoCorasickCache.h`), so results match
/// ClickHouse's legacy searcher exactly and this crate never needs Unicode tables.
pub struct AhoCorasickHandle {
    automaton: Option<DoubleArrayAhoCorasick<u32>>,
}

/// Builds an Aho-Corasick automaton from the given (already case-folded) patterns.
/// Returns null only on a genuine build failure; an empty pattern set yields a valid handle that
/// matches nothing. See `include/aho_corasick.h` for the parameter and de-duplication contract.
#[no_mangle]
pub unsafe extern "C" fn aho_corasick_create(
    patterns: *const *const u8,
    pattern_sizes: *const u64,
    num_patterns: u64,
) -> *mut AhoCorasickHandle {
    let result = panic::catch_unwind(|| {
        if num_patterns == 0 {
            return Box::into_raw(Box::new(AhoCorasickHandle { automaton: None }));
        }

        if patterns.is_null() || pattern_sizes.is_null() {
            return std::ptr::null_mut();
        }

        let num = num_patterns as usize;
        let pattern_ptrs = slice::from_raw_parts(patterns, num);
        let sizes = slice::from_raw_parts(pattern_sizes, num);

        let mut pattern_vec: Vec<&[u8]> = Vec::with_capacity(num);
        for i in 0..num {
            if pattern_ptrs[i].is_null() {
                return std::ptr::null_mut();
            }
            pattern_vec.push(slice::from_raw_parts(pattern_ptrs[i], sizes[i] as usize));
        }
        // daachorse's builder errors on duplicate patterns, so remove them first.
        pattern_vec.sort_unstable();
        pattern_vec.dedup();

        let automaton = match DoubleArrayAhoCorasickBuilder::new()
            .match_kind(MatchKind::LeftmostFirst)
            .build(&pattern_vec)
        {
            Ok(automaton) => Some(automaton),
            Err(_) => return std::ptr::null_mut(),
        };

        Box::into_raw(Box::new(AhoCorasickHandle { automaton }))
    });

    result.unwrap_or(std::ptr::null_mut())
}

/// Searches a batch of (already case-folded) haystacks for any pattern match, writing one result
/// byte per row. See `include/aho_corasick.h` for the ColumnString offset format and result encoding.
#[no_mangle]
pub unsafe extern "C" fn aho_corasick_search_batch(
    handle: *const AhoCorasickHandle,
    haystack_data: *const u8,
    haystack_offsets: *const u64,
    num_rows: u64,
    results: *mut u8,
) {
    let _ = panic::catch_unwind(|| {
        if handle.is_null()
            || haystack_data.is_null()
            || haystack_offsets.is_null()
            || results.is_null()
            || num_rows == 0
        {
            return;
        }

        let handle_ref = &*handle;
        let num = num_rows as usize;
        let offsets = slice::from_raw_parts(haystack_offsets, num);
        let results_slice = slice::from_raw_parts_mut(results, num);
        let Some(ac) = &handle_ref.automaton else {
            results_slice.fill(0);
            return;
        };

        let mut prev_offset: u64 = 0;
        for i in 0..num {
            let end_offset = offsets[i];

            if end_offset < prev_offset {
                results_slice[i] = 0;
                continue;
            }

            let start = prev_offset as usize;
            let end = end_offset as usize;
            let haystack = slice::from_raw_parts(haystack_data.add(start), end - start);

            results_slice[i] = u8::from(ac.leftmost_find_iter(haystack).next().is_some());
            prev_offset = end_offset;
        }
    });
}

/// Returns the automaton's heap footprint in bytes, used for cache sizing.
#[no_mangle]
pub unsafe extern "C" fn aho_corasick_heap_bytes(handle: *const AhoCorasickHandle) -> u64 {
    if handle.is_null() {
        return 0;
    }
    (*handle)
        .automaton
        .as_ref()
        .map_or(0, |automaton| automaton.heap_bytes() as u64)
}

/// Frees an automaton handle previously returned by `aho_corasick_create`.
#[no_mangle]
pub unsafe extern "C" fn aho_corasick_free(handle: *mut AhoCorasickHandle) {
    if !handle.is_null() {
        if let Err(_) = panic::catch_unwind(|| {
            drop(Box::from_raw(handle));
        }) {
            eprintln!("Warning: panic occurred while freeing Aho-Corasick automaton handle");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct TestAutomaton(*mut AhoCorasickHandle);

    impl Drop for TestAutomaton {
        fn drop(&mut self) {
            unsafe { aho_corasick_free(self.0) }
        }
    }

    fn create_automaton(patterns: &[&[u8]]) -> TestAutomaton {
        let pattern_ptrs: Vec<_> = patterns.iter().map(|pattern| pattern.as_ptr()).collect();
        let pattern_sizes: Vec<_> = patterns.iter().map(|pattern| pattern.len() as u64).collect();
        let handle = unsafe {
            aho_corasick_create(pattern_ptrs.as_ptr(), pattern_sizes.as_ptr(), patterns.len() as u64)
        };
        assert!(!handle.is_null(), "automaton creation failed");
        TestAutomaton(handle)
    }

    fn search_batch(automaton: &TestAutomaton, haystacks: &[&[u8]]) -> Vec<u8> {
        let mut haystack_data = Vec::new();
        let mut offsets = Vec::with_capacity(haystacks.len());
        for haystack in haystacks {
            haystack_data.extend_from_slice(haystack);
            offsets.push(haystack_data.len() as u64);
        }

        let mut results = vec![0; haystacks.len()];
        unsafe {
            aho_corasick_search_batch(
                automaton.0,
                haystack_data.as_ptr(),
                offsets.as_ptr(),
                haystacks.len() as u64,
                results.as_mut_ptr(),
            );
        }
        results
    }

    #[test]
    fn test_basic_search() {
        let automaton = create_automaton(&[b"hello", b"world"]);

        assert_eq!(
            search_batch(&automaton, &[b"hello\0", b"world\0", b"test\0"]),
            vec![1, 1, 0]
        );
        assert!(unsafe { aho_corasick_heap_bytes(automaton.0) } > 0);
    }

    #[test]
    fn test_duplicate_patterns_build_ok() {
        let automaton = create_automaton(&[b"abc", b"abc", b"def"]);

        assert_eq!(
            search_batch(&automaton, &[b"abc\0", b"def\0", b"xyz\0"]),
            vec![1, 1, 0]
        );
    }

    #[test]
    fn test_empty_pattern_set_matches_nothing() {
        let automaton = create_automaton(&[]);

        assert_eq!(search_batch(&automaton, &[b"abc\0", b"\0"]), vec![0, 0]);
        assert_eq!(unsafe { aho_corasick_heap_bytes(automaton.0) }, 0);
    }

    #[test]
    fn test_arbitrary_bytes_are_matched_verbatim() {
        let automaton = create_automaton(&[&[0xff, 0x00, 0x41]]);

        assert_eq!(
            search_batch(&automaton, &[&[0x78, 0xff, 0x00, 0x41, 0x78], b"nope\0"]),
            vec![1, 0]
        );
    }
}
