(block
  (pragma language overloads)
  (use qumirdb)
  ;; UPPER/LOWER of qumirdb.oz (qdb_string_case_into) on ASCII text: the
  ;; string goes through 16-byte <vec u8 16> blocks.
  (fun <main> ()
    (block
      (var n = 65536)
      (var buf = (cast (call array_create n) <ptr u8>))
      (var out = (cast (call array_create (+ n 64)) <ptr u8>))
      (var i = 0)
      (while (< i n)
        (block
          (= buf [i] (cast (+ 32 (% (* i 7) 95)) u8))
          (= i (+ i 1))))
      (var s = (cast (struct ((Data buf) (Size n))) StringView))
      (var total = 0)
      (var rep = 0)
      (while (< rep 400)
        (block
          (var size = (call qdb_string_case_into s (== (& rep 1) 0) out #f))
          (= total (+ total (+ size (cast (index out (& (* rep 131) (- n 1))) i64))))
          (= rep (+ rep 1))))
      (output total "\n"))))
