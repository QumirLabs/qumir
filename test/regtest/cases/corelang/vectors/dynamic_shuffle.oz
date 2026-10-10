(block
  (use Vector)
  (fun <main> ()
    (block
      (var values = (: (vec -128 -1 0 127) <vec i8 4>))
      (var i = 0)
      (while (< i 4)
        (block
          (var mask = (cast (vec i (% (+ i 1) 4) i (% (+ i 3) 4)) <vec u8 4>))
          (output (call builtin::shuffle values mask) "\n")
          (= i (+ i 1))))
      (var mask = (vec 1 0))
      (output (call builtin::shuffle (vec 1.5 2.5) mask) "\n")
      (output (call builtin::shuffle (vec #t #f) mask) "\n")
      (output (call builtin::shuffle (vec 4 5) (vec (index mask 0) 0)) "\n"))))
