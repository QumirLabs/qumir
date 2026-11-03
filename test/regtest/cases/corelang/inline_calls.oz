(block
  (use vector)
  (type Pair <struct (A i64) (B i64)>)

  (fun <main> ()
    (block
      ; multiple returns inside a loop: the result merges in a phi
      (var s i64)
      (= s 0)
      (var i i64)
      (= i 0)
      (while (< i 6)
        (block
          (= s (+ s (call clamp i 1 4)))
          (= i (+ i 1))))
      (output "clamp: " s "\n")

      ; the block after a short-circuit operand is split, the merge phi follows it
      (var both i64)
      (= both 0)
      (= i -2)
      (while (< i 8)
        (block
          (if (&& (call positive i) (call small i))
            (= both (+ both 1)))
          (= i (+ i 1))))
      (output "and: " both "\n")

      ; a struct goes in by value, a struct comes back
      (var p <named Pair>)
      (= p (call make 3 4))
      (output "make: " (field p A) " " (field p B) "\n")
      (output "swap sum: " (call swapSum p) " keeps " (field p A) " " (field p B) "\n")
      (var q <named Pair>)
      (= q (call swap (call swap p)))
      (output "swap twice: " (field q A) " " (field q B) "\n")

      ; nested inline functions and an argument computed by another inline call
      (output "nested: " (call twice (call twice 5)) " " (call quad 3) "\n")

      ; a return from nested loops leaves the copied body in the middle of a loop
      (var hits i64)
      (= hits 0)
      (= i 0)
      (while (< i 6)
        (block
          (= hits (+ (* hits 10) (call lookup i)))
          (= i (+ i 1))))
      (output "lookup: " hits "\n")

      ; recursive functions stay calls
      (output "fact: " (call fact 10) " even: " (call isEven 7) "\n")

      ; wide and vector values
      (var w = (call widen 1000000000000))
      (output "i128: " (cast (>> w (: 64 i128)) i64) " " (cast w i64) "\n")
      (output "vec: " (call addVec (: (vec 1 2 3 4) <vec i64 4>) (: (vec 10 20 30 40) <vec i64 4>)) "\n")

      ; strings and side effects
      (output (call greet "inline") "\n")
      (= i 0)
      (while (< i 3)
        (block
          (call say i)
          (= i (+ i 1))))))

  (fun clamp ((var x i64) (var lo i64) (var hi i64)) -> i64 (attrs inline)
    (block
      (if (< x lo)
        (return lo))
      (if (> x hi)
        (return hi))
      (return x)))

  (fun positive ((var x i64)) -> bool (attrs inline)
    (block (return (> x 0))))

  (fun small ((var x i64)) -> bool (attrs inline)
    (block (return (&& (> x 0) (< x 5)))))

  (fun make ((var a i64) (var b i64)) -> <named Pair> (attrs inline)
    (block
      (return (: (struct ((A a) (B b))) <named Pair>))))

  (fun swap ((var p <named Pair>)) -> <named Pair> (attrs inline)
    (block
      (return (: (struct ((A (field p B)) (B (field p A)))) <named Pair>))))

  (fun swapSum ((var p <named Pair>)) -> i64 (attrs inline)
    (block
      (= p (call swap p))
      (return (+ (* 10 (field p A)) (field p B)))))

  (fun twice ((var x i64)) -> i64 (attrs inline)
    (block (return (* x 2))))

  (fun quad ((var x i64)) -> i64 (attrs inline)
    (block (return (call twice (call twice x)))))

  (fun lookup ((var h i64)) -> i64 (attrs inline)
    (block
      (var slot = (call probe h))
      (if (>= slot 0)
        (return slot))
      (return 5)))

  (fun probe ((var h i64)) -> i64 (attrs inline)
    (block
      (var g = (& h 3))
      (var probes i64)
      (= probes 0)
      (while (< probes 4)
        (block
          (var m = (+ g 1))
          (while (!= m 0)
            (block
              (if (== (+ g m) (+ (& h 1) 3))
                (return (+ g m)))
              (= m (- m 1))))
          (= g (& (+ g 1) 3))
          (= probes (+ probes 1))))
      (return -1)))

  (fun fact ((var n i64)) -> i64 (attrs inline)
    (block
      (if (<= n 1)
        (return 1))
      (return (* n (call fact (- n 1))))))

  (fun isEven ((var n i64)) -> bool (attrs inline)
    (block
      (if (== n 0)
        (return #t))
      (return (call isOdd (- n 1)))))

  (fun isOdd ((var n i64)) -> bool (attrs inline)
    (block
      (if (== n 0)
        (return #f))
      (return (call isEven (- n 1)))))

  (fun widen ((var x i64)) -> i128 (attrs inline)
    (block (return (* (cast x i128) (: 1000000000 i128)))))

  (fun addVec ((var a <vec i64 4>) (var b <vec i64 4>)) -> <vec i64 4> (attrs inline)
    (block (return (+ a b))))

  (fun greet ((var name string)) -> string (attrs inline)
    (block (return (+ "hello, " name))))

  (fun say ((var x i64)) (attrs inline)
    (block
      (if (== x 1)
        (block
          (output "one\n")
          (return)))
      (output "say " x "\n"))))
