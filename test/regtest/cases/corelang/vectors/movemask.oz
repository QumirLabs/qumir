(block
  (use Vector)
  (fun <main> ()
    (block
      (output (call builtin::movemask (vec #t #f)) "\n")
      (output (call builtin::movemask (vec #f #t #f #t)) "\n")
      (var bytes = (: (vec 7 0 7 7 0 0 0 7 7 0 0 7 0 7 0 7) <vec u8 16>))
      (output (call builtin::movemask (== bytes 7)) "\n")
      (output (call builtin::movemask (vec #t #t #t #t #t #t #t #t #t #t #t #t #t #t #t #t
                                           #t #t #t #t #t #t #t #t #t #t #t #t #t #t #t #t)) "\n")
      (output (call builtin::movemask (vec #f #f #f #f)) "\n"))))
