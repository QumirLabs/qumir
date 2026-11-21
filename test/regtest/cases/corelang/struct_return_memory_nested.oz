(block
  ; Nested structs over the FP return registers and a vector field, both
  ; returned through the sret buffer of the caller.
  (type pt <struct (x i64) (y f64)>)
  (type path <struct (p0 pt) (p1 pt) (p2 pt) (p3 pt) (p4 pt) (p5 pt) (p6 pt) (p7 pt) (p8 pt) (closed bool)>)
  (type lanes <struct (v <vec i64 8>) (n i64)>)

  (fun <main> ()
    (block
      (var r = (call line 2 0.5))
      (output (field (field r p0) x) " " (field (field r p8) x) " " (field (field r p8) y) " " (field r closed) "\n")
      (output (call length (call line 3 0.25)) "\n")
      (var c = (call same (call line 1 1.0)))
      (output (field c closed) " " (field (field c p4) y) "\n")
      (var l = (call iota 10))
      (output (index (field l v) 0) " " (index (field l v) 7) " " (field l n) "\n")))

  (fun at ((var k i64) (var s f64)) -> pt
    (block
      (return (: (struct ((x k) (y (* (cast k f64) s)))) pt))))

  (fun line ((var k i64) (var s f64)) -> path
    (block
      (return (: (struct ((p0 (call at 0 s)) (p1 (call at k s)) (p2 (call at (* 2 k) s))
        (p3 (call at (* 3 k) s)) (p4 (call at (* 4 k) s)) (p5 (call at (* 5 k) s))
        (p6 (call at (* 6 k) s)) (p7 (call at (* 7 k) s)) (p8 (call at (* 8 k) s)) (closed (> k 1)))) path))))

  (fun same ((var p path)) -> path
    (block
      (return p)))

  (fun length ((var p path)) -> f64
    (block
      (return (+ (field (field p p8) y) (cast (field (field p p8) x) f64)))))

  (fun iota ((var k i64)) -> lanes
    (block
      (return (: (struct ((v (vec k (+ k 1) (+ k 2) (+ k 3) (+ k 4) (+ k 5) (+ k 6) (+ k 7))) (n 8))) lanes)))))
