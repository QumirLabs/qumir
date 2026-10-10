(block
  (use Vector)
  (fun <main> ()
    (block
      (var a = (: (vec -128 -1 0 127) <vec i8 4>))
      (output (call builtin::all (== a a)) " " (call builtin::any (< a 0)) "\n")
      (output (call builtin::all (vec #f #f)) " " (call builtin::any (vec #f #f)) "\n")
      (output (call builtin::all (vec #t #t)) " " (call builtin::any (vec #t #t)) "\n")
      (output (call builtin::shuffle a (vec 3 2 1 0)) "\n")
      (output (call builtin::shuffle a (vec 0 0 3 3)) "\n")
      (output (call builtin::shuffle (vec 1.5 2.5) (vec 1 0)) "\n")
      (output (call builtin::shuffle (vec #t #f) (vec 1 1)) "\n")
      (var flags = (vec #t #t #t #t #t #t #t #t #t #t #t #t #t #t #t #t
                        #t #t #t #t #t #t #t #t #t #t #t #t #t #t #t #f))
      (output (call builtin::all flags) " " (call builtin::any flags) "\n")
      (output (call builtin::any (! flags)) "\n"))))
