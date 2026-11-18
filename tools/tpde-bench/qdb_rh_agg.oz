(block
  ;; Group-by through the robin hood kernels of qdb (aggregation/insert_fixed.oz,
  ;; aggregation/lookup.oz): insert every key, then look every row up and sum.
  (fun <main> ()
    (block
      (var cap = 131072)
      (var keys = (cast (call array_create (* cap 8)) <ptr i64>))
      (var dist = (cast (call array_create (* cap 8)) <ptr i64>))
      (var sums = (cast (call array_create (* cap 8)) <ptr i64>))
      (var i = 0)
      (while (< i cap)
        (block
          (= dist [i] (: -1 i64))
          (= i (+ i 1))))
      (var n = 1000000)
      (var seed = (: 12345 u64))
      (= i 0)
      (while (< i n)
        (block
          (= seed (+ (* seed (: 6364136223846793005 u64)) (: 1442695040888963407 u64)))
          (var key = (cast (% (>> seed (: 33 u64)) (: 50000 u64)) i64))
          (if (< (call aggregation_insert_fixed keys dist cap key) 0)
            (block (output "full\n")))
          (= i (+ i 1))))
      (= seed (: 12345 u64))
      (= i 0)
      (while (< i n)
        (block
          (= seed (+ (* seed (: 6364136223846793005 u64)) (: 1442695040888963407 u64)))
          (var key = (cast (% (>> seed (: 33 u64)) (: 50000 u64)) i64))
          (var slot = (call aggregation_lookup keys dist cap key))
          (= sums [slot] (+ (index sums slot) (& i 1023)))
          (= i (+ i 1))))
      (var groups = 0)
      (var check = 0)
      (= i 0)
      (while (< i 60000)
        (block
          (var slot = (call aggregation_lookup keys dist cap i))
          (if (>= slot 0)
            (block
              (= groups (+ groups 1))
              (= check (+ (* check 31) (index sums slot)))))
          (= i (+ i 1))))
      (output groups " " check "\n"))))
