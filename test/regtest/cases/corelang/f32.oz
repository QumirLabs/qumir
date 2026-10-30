(block
  (type Point <struct (X f32) (Y f32)>)

  (fun <main> ()
    (block
      (var x f32)
      (= x (: 1.5 f32))
      (var y f64)
      (= y x)
      (var sum = (+ x 2.25))
      (var narrowed = (cast (: 0.1 f64) f32))
      (var rounded = (cast 16777217 f32))
      (var values <array f32 1> [0 2])
      (= values [0] x)
      (= values [1] (call half x))
      (= values [2] (- x))
      (var total = (: 0.0 f32))
      (var i = 0)
      (for i 0 2 1
        (block (= total (+ total (index values i)))))
      (var p = (: (struct ((X (: 1.25 f32)) (Y (: 2.5 f32)))) Point))
      (output y " " sum " " (* x 2) " " (cast narrowed f64) " " (cast rounded i64) "\n")
      (output total " " (< x sum) " " (== x (: 1.5 f64)) " " (cast sum i64) " " (! x) "\n")
      (output (+ (field p X) (field p Y)) " " (cast (field p Y) i64) "\n")))

  (fun half ((var x f32)) -> f32
    (block (/ x 2))))
