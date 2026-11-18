(block
  (type Dec <struct (v i128) (valid bool)>)

  (fun <main> ()
    (block
      (var n i64)
      (= n (: 2000000 i64))
      (var acc = (call make_dec (: 0 i64)))
      (var mx i128)
      (= mx (: 0 i128))
      (var nulls i64)
      (= nulls (: 0 i64))
      (var i i64)
      (= i (: 0 i64))
      (while (< i n)
        (block
          (var price = (call make_dec (+ (* (% i (: 1000 i64)) (: 125 i64)) (: 99 i64))))
          (var qty = (call make_dec (+ (% i (: 50 i64)) (: 1 i64))))
          (var disc = (call make_dec (% i (: 10 i64))))
          (if (== (% i (: 97 i64)) (: 0 i64))
            (block (= disc (call null_dec))))
          (var line = (call dec_mul (call dec_mul price qty) (call dec_sub (call make_dec (: 100 i64)) disc)))
          (if (field line valid)
            (block
              (= acc (call dec_add acc line))
              (if (> (field line v) mx) (block (= mx (field line v)))))
            (block (= nulls (+ nulls (: 1 i64)))))
          (= i (+ i (: 1 i64)))))
      (output (cast (>> (field acc v) (: 64 i128)) i64) " " (cast (field acc v) i64) " "
        (cast mx i64) " " nulls "\n")))

  (fun make_dec ((var x i64)) -> Dec
    (block
      (return (: (struct ((v (* (cast x i128) (: 10000 i128))) (valid #t))) Dec))))

  (fun null_dec () -> Dec
    (block
      (return (: (struct ((v (: 0 i128)) (valid #f))) Dec))))

  (fun dec_add ((var a Dec) (var b Dec)) -> Dec
    (block
      (return (: (struct ((v (+ (field a v) (field b v))) (valid (&& (field a valid) (field b valid))))) Dec))))

  (fun dec_sub ((var a Dec) (var b Dec)) -> Dec
    (block
      (return (: (struct ((v (- (field a v) (field b v))) (valid (&& (field a valid) (field b valid))))) Dec))))

  (fun dec_mul ((var a Dec) (var b Dec)) -> Dec
    (block
      (var p = (* (field a v) (field b v)))
      ; rescale without i128 division: shift by a power of two instead
      (return (: (struct ((v (>> p (: 13 i128))) (valid (&& (field a valid) (field b valid))))) Dec)))))
