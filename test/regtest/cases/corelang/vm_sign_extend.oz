(block
  (type S <struct (a i8) (b i16) (c i32) (d u8) (e u16) (f u32)>)

  (fun <main> ()
    (block
      (var small <array i8 1> [0 0])
      (var mid <array i16 1> [0 0])
      (var wide <array i32 1> [0 0])
      (var v S)
      (= small [0] (: -128 i8))
      (= mid [0] (: -300 i16))
      (= wide [0] (: -7 i32))
      (= v (: (struct ((a (: -5 i8)) (b (: -300 i16)) (c (: -7 i32))
                       (d (: 255 u8)) (e (: 65535 u16)) (f (: 4294967295 u32)))) S))
      (output (call load_i8 (cast small <ptr i8>)) " "
        (call load_i16 (cast mid <ptr i16>)) " "
        (call load_i32 (cast wide <ptr i32>)) " "
        (% (call load_i32 (cast wide <ptr i32>)) 2) "\n")
      (output (cast (field v a) i64) " "
        (cast (field v b) i64) " "
        (cast (field v c) i64) "\n")
      (output (cast (field v d) i64) " "
        (cast (field v e) i64) " "
        (cast (field v f) i64) "\n")))
  (fun load_i8 ((var p <ptr i8>)) -> i64 (block (return (cast (index p 0) i64))))
  (fun load_i16 ((var p <ptr i16>)) -> i64 (block (return (cast (index p 0) i64))))
  (fun load_i32 ((var p <ptr i32>)) -> i64 (block (return (cast (index p 0) i64)))))
