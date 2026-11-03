; Parameters live in the callee's registers: assigned, address-taken, struct, wide and vector ones.
(block
  (use vector)
  (type pair <struct (a i64) (b i64)>)

  (fun <main> ()
    (block
      (var p pair)
      (= p (: (struct ((a (: 3 i64)) (b (: 4 i64)))) pair))
      (output (call mixed (: 1 i64) (: -5 i128) (: 2.5 f64) (: 7 i64) p) "\n")
      (output (call assigned (: 10 i64) (: 20 i128) (call assigned (: 1 i64) (: 2 i128) (: 3 i64) (: 0 i64)) (: 4 i64)) "\n")
      (output (call taken (: 5 i64)) "\n")
      (output (call by_value p) " " (field p a) " " (field p b) "\n")
      (output (call fib (: 15 i64)) " " (call sum_to (: 100 i64) (: 0 i64)) "\n")
      (output (call lanes (vec 1 2 3 4) (call lanes (vec 10 20 30 40) (vec 1 1 1 1) (: 5 i64)) (: 1 i64)) "\n")))

  ; narrow and wide parameters of different alignment
  (fun mixed ((var a i64) (var w i128) (var f f64) (var n i64) (var s pair)) -> i64
    (block
      (return (+ (+ a (cast w i64)) (+ (+ (cast f i64) n) (+ (field s a) (field s b)))))))

  ; parameters that are stored to
  (fun assigned ((var a i64) (var w i128) (var c i64) (var d i64)) -> i64
    (block
      (= a (+ a c))
      (= w (* w (: 3 i128)))
      (= d (+ d a))
      (return (+ (+ a (cast w i64)) d))))

  ; a parameter whose address is passed on
  (fun taken ((var x i64)) -> i64
    (block
      (call bump x)
      (call bump x)
      (return x)))
  (fun bump ((var x <ref i64>))
    (block (= x (+ x (: 7 i64)))))

  ; the callee changes its copy, not the caller's struct
  (fun by_value ((var s pair)) -> i64
    (block
      (= s (: (struct ((a (+ (field s a) (: 100 i64))) (b (: 0 i64)))) pair))
      (return (+ (field s a) (field s b)))))

  (fun fib ((var n i64)) -> i64
    (block
      (if (< n (: 2 i64)) (block (return n)))
      (return (+ (call fib (- n (: 1 i64))) (call fib (- n (: 2 i64)))))))
  (fun sum_to ((var n i64) (var acc i64)) -> i64
    (block
      (if (== n (: 0 i64)) (block (return acc)))
      (return (call sum_to (- n (: 1 i64)) (+ acc n)))))

  (fun lanes ((var x <vec i64 4>) (var y <vec i64 4>) (var k i64)) -> <vec i64 4>
    (block
      (= x (+ x y))
      (= k (+ k (: 1 i64)))
      (return (+ x (vec k k k k))))))
