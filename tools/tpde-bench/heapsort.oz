(block
  (fun <main> ()
    (block
      (var n i64)
      (= n (: 300000 i64))
      (var a <ptr i64>)
      (= a (cast (call array_create (* n (: 8 i64))) <ptr i64>))
      (var seed u64)
      (= seed (: 12345 u64))
      (var i i64)
      (= i (: 0 i64))
      (while (< i n)
        (block
          (= seed (+ (* seed (: 6364136223846793005 u64)) (: 1442695040888963407 u64)))
          (= a [i] (cast (>> seed (: 33 u64)) i64))
          (= i (+ i (: 1 i64)))))
      (call heapsort a n)
      (var sorted i64)
      (= sorted (: 1 i64))
      (var sum i64)
      (= sum (: 0 i64))
      (= i (: 1 i64))
      (while (< i n)
        (block
          (if (> (index a (- i (: 1 i64))) (index a i)) (block (= sorted (: 0 i64))))
          (= sum (+ (* sum (: 31 i64)) (index a i)))
          (= i (+ i (: 1 i64)))))
      (output sorted " " sum " " (index a (: 0 i64)) " " (index a (- n (: 1 i64))) "\n")))

  (fun sift ((var a <ptr i64>) (var start i64) (var stop i64))
    (block
      (var root i64)
      (= root start)
      (while (<= (+ (* root (: 2 i64)) (: 1 i64)) stop)
        (block
          (var child = (+ (* root (: 2 i64)) (: 1 i64)))
          (var swap = root)
          (if (< (index a swap) (index a child)) (block (= swap child)))
          (if (<= (+ child (: 1 i64)) stop)
            (block
              (if (< (index a swap) (index a (+ child (: 1 i64))))
                (block (= swap (+ child (: 1 i64)))))))
          (if (== swap root) (block (return)))
          (var t = (index a root))
          (= a [root] (index a swap))
          (= a [swap] t)
          (= root swap)))))

  (fun heapsort ((var a <ptr i64>) (var n i64))
    (block
      (var start = (/ (- n (: 2 i64)) (: 2 i64)))
      (while (>= start (: 0 i64))
        (block
          (call sift a start (- n (: 1 i64)))
          (= start (- start (: 1 i64)))))
      (var stop = (- n (: 1 i64)))
      (while (> stop (: 0 i64))
        (block
          (var t = (index a stop))
          (= a [stop] (index a (: 0 i64)))
          (= a [(: 0 i64)] t)
          (= stop (- stop (: 1 i64)))
          (call sift a (: 0 i64) stop))))))
