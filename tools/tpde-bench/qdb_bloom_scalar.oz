(block
  (pragma language overloads)
  (use qumirdb)
  ;; Bloom filter probe of qdb (join/runtime_filter.oz), scalar variant:
  ;; 1 MiB of filter words, 8 probes of 1M hashes, half of them added.
  (fun <main> ()
    (block
      (var word_count = 131072)
      (var words = (cast (call array_create (* word_count 8)) <ptr u64>))
      (var block_mask = (call rf_bloom_block_mask word_count))
      (var count = 1048576)
      (var hashes = (cast (call array_create (* count 8)) <ptr u64>))
      (var output = (cast (call array_create count) <ptr u8>))
      (var i = 0)
      (while (< i count)
        (block
          (= hashes [i] (call rf_mix (cast i u64)))
          (= i (+ i 1))))
      (call rf_bloom_add words block_mask hashes (cast 0 <ptr u8>) (>> count 1))
      (var kept = 0)
      (var rep = 0)
      (while (< rep 8)
        (block
          (= kept (+ kept (call rf_bloom_probe_scalar words block_mask hashes
                            (cast 0 <ptr u8>) output count)))
          (= rep (+ rep 1))))
      (output kept "\n"))))
