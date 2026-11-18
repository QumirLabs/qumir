(block
  ;; radix_sort [i64] of qdb (sort/radix.oz): eight 8-bit counting passes.
  (fun <main> ()
    (block
      (var n = 400000)
      (var values <array i64 1> [0 399999])
      (var seed = (: 12345 u64))
      (var i = 0)
      (while (< i n)
        (block
          (= seed (+ (* seed (: 6364136223846793005 u64)) (: 1442695040888963407 u64)))
          (= values [i] (- (cast (>> seed (: 1 u64)) i64) (: 4611686018427387904 i64)))
          (= i (+ i 1))))
      (call radix_sort n values)
      (var sorted = 1)
      (var check = 0)
      (= i 1)
      (while (< i n)
        (block
          (if (> (index values (- i 1)) (index values i)) (block (= sorted 0)))
          (= check (+ (* check 31) (>> (index values i) 20)))
          (= i (+ i 1))))
      (output sorted " " check " " (index values 0) " " (index values (- n 1)) "\n"))))
