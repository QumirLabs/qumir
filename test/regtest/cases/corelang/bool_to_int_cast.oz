(block
  (fun <main> ()
    (block
      (var x i64)
      (var small u8)
      (var wide i64)
      (var real f64)
      (= x 3)
      (= small (cast (> x 2) u8))
      (= wide (cast (> x 2) i64))
      (= real (cast (> x 2) f64))
      (output small " " wide " " real " " (cast (> x 5) u8) " " (cast (> x 2) u16) "\n"))))
