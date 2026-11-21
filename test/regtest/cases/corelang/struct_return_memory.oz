(block
  ; Results with more parts than the return registers (8 general and 8 FP on
  ; AArch64) come back through a buffer of the caller, the sret argument.
  (type nine <struct (a i64) (b i64) (c i64) (d i64) (e i64) (f i64) (g i64) (h i64) (i i64)>)
  ; 288 bytes: copied with memcpy.
  (type four <struct (w nine) (x nine) (y nine) (z nine)>)

  (fun <main> ()
    (block
      (var p = (call mk 1))
      (output (field p a) " " (field p e) " " (field p i) "\n")
      (output (call sum (call mk 10)) "\n")
      (output (call sum (call pick #t)) " " (call sum (call pick #f)) "\n")
      (var f = (call fib 10))
      (output (field f a) " " (field f b) " " (field f i) "\n")
      (var q = (call spread 100))
      (output (field (field q w) a) " " (field (field q x) e) " " (field (field q y) h) " " (field (field q z) i) "\n")
      (output (call sum (field (call spread 7) z)) "\n")))

  (fun mk ((var k i64)) -> nine
    (block
      (return (: (struct ((a k) (b (+ k 1)) (c (+ k 2)) (d (+ k 3)) (e (+ k 4))
        (f (+ k 5)) (g (+ k 6)) (h (+ k 7)) (i (+ k 8)))) nine))))

  (fun sum ((var p nine)) -> i64
    (block
      (return (+ (field p a) (field p b) (field p c) (field p d) (field p e)
        (field p f) (field p g) (field p h) (* 1000 (field p i))))))

  (fun pick ((var c bool)) -> nine
    (block
      (if c
        (block (return (call mk 1)))
        (block (return (call mk 2))))))

  ; a and b are fib(n) and fib(n - 1), i counts the calls.
  (fun fib ((var n i64)) -> nine
    (block
      (if (== n 1)
        (block (return (: (struct ((a 1) (b 0) (c 0) (d 0) (e 0) (f 0) (g 0) (h 0) (i 1))) nine))))
      (var p = (call fib (- n 1)))
      (return (: (struct ((a (+ (field p a) (field p b))) (b (field p a)) (c 0) (d 0) (e 0)
        (f 0) (g 0) (h 0) (i (+ (field p i) 1)))) nine))))

  (fun spread ((var k i64)) -> four
    (block
      (return (: (struct ((w (call mk k)) (x (call mk (+ k 10))) (y (call mk (+ k 20))) (z (call mk (+ k 30))))) four)))))
