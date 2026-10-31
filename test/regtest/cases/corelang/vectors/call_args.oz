(block
  (use vector)
  (fun <main> ()
    (block
      (var a = (: (vec 1 2 3 4) <vec u64 4>))
      (var b = (: (vec 10 20 30 40) <vec u64 4>))
      (output (call add a b) "\n")
      (output (cast (call lane0 (call add b a)) i64) " " (cast (call lane3 (: 100 u64) b) i64) " " (call half (vec 1.5 2.5)) "\n")
      (var wide = (: (vec 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16) <vec u16 16>))
      (output (cast (call lane15 wide) i64) "\n")))
  (fun add ((var x <vec u64 4>) (var y <vec u64 4>)) -> <vec u64 4>
    (block (return (+ x y))))
  (fun lane0 ((var x <vec u64 4>)) -> u64
    (block (return (index x 0))))
  (fun lane3 ((var k u64) (var x <vec u64 4>)) -> u64
    (block (return (+ k (index x 3)))))
  (fun half ((var x <vec f64 2>)) -> f64
    (block (return (index x 1))))
  (fun lane15 ((var x <vec u16 16>)) -> u16
    (block (return (index x 15)))))
