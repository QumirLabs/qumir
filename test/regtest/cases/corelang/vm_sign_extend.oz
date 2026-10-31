(block
  (fun load_i8 ((var p <ptr i8>)) -> i64 (block (return (cast (index p 0) i64))))
  (fun load_i16 ((var p <ptr i16>)) -> i64 (block (return (cast (index p 0) i64))))
  (fun load_i32 ((var p <ptr i32>)) -> i64 (block (return (cast (index p 0) i64))))
  (fun <main> ()
    (block
      (var small <array i8 1> [0 0])
      (var mid <array i16 1> [0 0])
      (var wide <array i32 1> [0 0])
      (= small [0] (: -128 i8))
      (= mid [0] (: -300 i16))
      (= wide [0] (: -7 i32))
      (output (call load_i8 (cast small <ptr i8>)) " "
        (call load_i16 (cast mid <ptr i16>)) " "
        (call load_i32 (cast wide <ptr i32>)) " "
        (% (call load_i32 (cast wide <ptr i32>)) 2) "\n"))))
