(block
  ; symbols defined in test_reg.cpp; structs that go to the stack after the
  ; argument registers run out
  (type ii <struct (a i64) (b i64)>)
  (type ss <struct (a f64) (b f64)>)
  (type is <struct (a i64) (b f64)>)
  (type wide <struct (a i64) (b i64) (c i64)>)

  (fun <main> ()
    (block
      (var p_ii ii)
      (= p_ii (: (struct ((a 1) (b 2))) ii))
      (var p_ss ss)
      (= p_ss (: (struct ((a 3.0) (b 4.0))) ss))
      (var p_is is)
      (= p_is (: (struct ((a 5) (b 6.0))) is))
      (var p_wide wide)
      (= p_wide (: (struct ((a 7) (b 8) (c 9))) wide))
      (output "mix: "
        (call qumir_stack_mix 1 2 3 4 5 6 7 8 1.0 2.0 3.0 4.0 5.0 6.0 7.0 8.0 p_ii p_ss p_is p_wide 1 2.0)
        "\n")

      (var q_ii ii)
      (= q_ii (: (struct ((a 3) (b 4))) ii))
      (var r_ii ii)
      (= r_ii (: (struct ((a 5) (b 6))) ii))
      (var s_ii ii)
      (= s_ii (: (struct ((a 7) (b 8))) ii))
      (output "regs: " (call qumir_regs_ii p_ii q_ii r_ii s_ii 9) "\n")

      (var big wide)
      (= big (: (struct ((a 10) (b 20) (c 30))) wide))
      (var r_wide wide)
      (= r_wide (call qumir_stack_wide 1 2 3 4 5 6 7 8 big))
      (output "wide: " (field r_wide a) " " (field r_wide b) " " (field r_wide c) "\n")))

  (fun qumir_stack_mix (
      (var a0 i64) (var a1 i64) (var a2 i64) (var a3 i64)
      (var a4 i64) (var a5 i64) (var a6 i64) (var a7 i64)
      (var d0 f64) (var d1 f64) (var d2 f64) (var d3 f64)
      (var d4 f64) (var d5 f64) (var d6 f64) (var d7 f64)
      (var p_ii ii) (var p_ss ss) (var p_is is) (var p_wide wide) (var t i64) (var u f64))
    -> i64 (attrs extern) (block))
  (fun qumir_regs_ii ((var p ii) (var q ii) (var r ii) (var s ii) (var t i64)) -> i64 (attrs extern) (block))
  (fun qumir_stack_wide (
      (var a0 i64) (var a1 i64) (var a2 i64) (var a3 i64)
      (var a4 i64) (var a5 i64) (var a6 i64) (var a7 i64)
      (var p wide))
    -> wide (attrs extern) (block)))
