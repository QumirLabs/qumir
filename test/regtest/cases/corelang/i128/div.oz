(block
  ; 128-bit '//' and '%' truncate toward zero; x // 0 and x % 0 give 0,
  ; MIN // -1 gives MIN and MIN % -1 gives 0.
  (fun <main> ()
    (block
      (var min = (<< (: 1 i128) (: 127 i128)))
      (var max = (~ min))
      (var big = (* (: 1000000000000 i128) (: 1000000000 i128)))
      (var wide = (* big (: 1000000000 i128)))

      (output "big / 7:      ") (call sdivrem big (: 7 i128))
      (output "-big / 7:     ") (call sdivrem (- big) (: 7 i128))
      (output "big / -7:     ") (call sdivrem big (: -7 i128))
      (output "-big / -7:    ") (call sdivrem (- big) (: -7 i128))
      ; both operands over 64 bits
      (output "wide / big+1: ") (call sdivrem wide (+ big (: 1 i128)))
      (output "big / 0:      ") (call sdivrem big (: 0 i128))
      (output "min / -1:     ") (call sdivrem min (: -1 i128))
      (output "min / 1:      ") (call sdivrem min (: 1 i128))
      (output "min / max:    ") (call sdivrem min max)
      (output "max / min:    ") (call sdivrem max min)
      (output "min / min:    ") (call sdivrem min min)

      ; the top bit of an unsigned value is a digit, not a sign
      (var umax = (~ (: 0 u128)))
      (var top = (cast min u128))
      (output "umax / 10:    ") (call udivrem umax (: 10 u128))
      (output "umax / top:   ") (call udivrem umax top)
      (output "top / umax:   ") (call udivrem top umax)
      (output "umax / 0:     ") (call udivrem umax (: 0 u128))
      (output "umax / 1:     ") (call udivrem umax (: 1 u128))))

  (fun sdivrem ((var a i128) (var b i128))
    (block
      (call put (// a b))
      (output " ")
      (call put (% a b))
      (output "\n")))

  (fun udivrem ((var a u128) (var b u128))
    (block
      (call uput (// a b))
      (output " ")
      (call uput (% a b))
      (output "\n")))

  ; Decimal digits through the division itself; the remainders of a negative
  ; value are negative, so MIN needs no negation.
  (fun put ((var x i128))
    (block
      (if (< x (: 0 i128))
        (block (output "-")))
      (call digits x)))

  (fun digits ((var x i128))
    (block
      (var rest = (// x (: 10 i128)))
      (if (!= rest (: 0 i128))
        (block (call digits rest)))
      (var d = (cast (% x (: 10 i128)) i64))
      (if (< d 0)
        (block (= d (- d))))
      (output d)))

  (fun uput ((var x u128))
    (block
      (var rest = (// x (: 10 u128)))
      (if (!= rest (: 0 u128))
        (block (call uput rest)))
      (output (cast (% x (: 10 u128)) i64)))))
