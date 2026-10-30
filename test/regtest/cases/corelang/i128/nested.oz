; A struct holding an i128 is 16-aligned when it is itself a field: after a
; narrow field it sits at 16, and field access must agree with struct copies.
(block
  (type Inner <struct (mark i8) (wide i128)>)
  (type Outer <struct (flag i8) (inner Inner) (tail i64)>)

  (fun <main> ()
    (block
      (var base i128)
      (= base (* (: 1000000000000 i128) (: 1000000000 i128)))

      (var v Outer)
      (= v (: (struct ((flag (: 6 i8))
                       (inner (: (struct ((mark (: 9 i8)) (wide base))) Inner))
                       (tail (: 43 i64))))
            Outer))
      (call show "in:   " v)

      ; by value into a function and back out as a return value
      (var r Outer)
      (= r (call bump v))
      (call show "ret:  " r)

      ; by reference: the callee writes the nested 128 bits back
      (call scale v)
      (call show "ref:  " v)

      ; an array of them: every element keeps its own nested i128
      (var arr <array Outer 1> [0 2])
      (= arr [0] v)
      (= arr [1] r)
      (= arr [2] (call bump r))
      (call show "arr0: " (index arr (: 0 i64)))
      (call show "arr2: " (index arr (: 2 i64)))))

  (fun show ((var name string) (var s Outer))
    (block
      (output name (cast (field s flag) i64)
        " " (cast (field (field s inner) mark) i64)
        " " (cast (>> (field (field s inner) wide) (: 64 i128)) i64)
        " " (cast (field (field s inner) wide) i64)
        " " (field s tail) "\n")))

  (fun bump ((var s Outer)) -> Outer
    (block
      (return (: (struct
                   ((flag (+ (field s flag) (: 1 i8)))
                    (inner (: (struct ((mark (field (field s inner) mark))
                                       (wide (+ (field (field s inner) wide) (field (field s inner) wide)))))
                            Inner))
                    (tail (+ (field s tail) (: 1 i64)))))
              Outer))))

  (fun scale ((var p <ref Outer>))
    (block
      (= p (: (struct
                ((flag (field p flag))
                 (inner (: (struct ((mark (field (field p inner) mark))
                                    (wide (* (field (field p inner) wide) (: 3 i128)))))
                         Inner))
                 (tail (field p tail))))
           Outer)))))
