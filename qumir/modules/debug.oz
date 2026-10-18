; Keep checkpoint state in WASM to avoid crossing into JS for non-stopping points.
(block
  (var __qumir_debug_enabled i64)
  (var __qumir_debug_mode_state i64)
  (var __qumir_debug_target i64)
  (var __qumir_debug_depth_state i64)
  (var __qumir_debug_next_frame i64)
  (var __qumir_debug_capacity i64)
  (var __qumir_debug_stride_state i64)
  (var __qumir_debug_point_count i64)
  (var __qumir_debug_points <ptr i64>)
  (var __qumir_debug_frames <ptr i64>)

  (fun __qumir_debug_alloc ((var size i64)) -> <ptr i64>
    (attrs (extern array_create)) (block))
  (fun __qumir_debug_free ((var address <ptr i64>)) -> void
    (attrs (extern array_destroy)) (block))
  ; Semantically returns <future void>; keep the raw handle so point can forward it
  ; without awaiting in its body and becoming a coroutine itself.
  (fun __qumir_debug_slow ((var id i32) (var address <ptr void>) (var reason i32)) -> <ptr void>
    (attrs extern) (block))

  (fun __qumir_debug_dispose () -> void
    (block
      (= __qumir_debug_enabled 0)
      (if (!= (cast __qumir_debug_points i64) 0) (call __qumir_debug_free __qumir_debug_points))
      (if (!= (cast __qumir_debug_frames i64) 0) (call __qumir_debug_free __qumir_debug_frames))
      (= __qumir_debug_points (cast 0 <ptr i64>))
      (= __qumir_debug_frames (cast 0 <ptr i64>))
      (= __qumir_debug_depth_state 0)))

  (fun __qumir_debug_init ((var count i32) (var locals i32)) -> <ptr i64>
    (block
      (call __qumir_debug_dispose)
      (= __qumir_debug_point_count (cast count i64))
      (= __qumir_debug_stride_state (+ 3 (cast locals i64)))
      (= __qumir_debug_capacity 16)
      (= __qumir_debug_points (call __qumir_debug_alloc (* __qumir_debug_point_count 40)))
      (= __qumir_debug_frames (call __qumir_debug_alloc (* (* __qumir_debug_capacity __qumir_debug_stride_state) 8)))
      (= __qumir_debug_mode_state 1)
      (= __qumir_debug_target 0)
      (= __qumir_debug_next_frame 0)
      (= __qumir_debug_enabled 1)
      (return __qumir_debug_points)))

  (fun __qumir_debug_control ((var mode i32) (var target i64)) -> void
    (block
      (= __qumir_debug_mode_state (cast mode i64))
      (= __qumir_debug_target target)))

  (fun __qumir_debug_breakpoint ((var line i32) (var enabled i32)) -> void
    (block
      (var i = 0)
      (while (< i __qumir_debug_point_count)
        (block
          (var base = (* i 5))
          (if (&& (== (index __qumir_debug_points base) 4)
                  (== (index __qumir_debug_points (+ base 2)) (cast line i64)))
            (= __qumir_debug_points [(+ base 4)] (cast enabled i64)))
          (= i (+ i 1))))))

  (fun __qumir_debug_frame_data () -> <ptr i64>
    (block (return __qumir_debug_frames)))
  (fun __qumir_debug_depth () -> i32
    (block (return (cast __qumir_debug_depth_state i32))))

  (fun __qumir_debug_grow () -> void
    (block
      (var size = (* __qumir_debug_capacity __qumir_debug_stride_state))
      (var frames = (call __qumir_debug_alloc (* size 16)))
      (var i = 0)
      (while (< i size)
        (block
          (= frames [i] (index __qumir_debug_frames i))
          (= i (+ i 1))))
      (call __qumir_debug_free __qumir_debug_frames)
      (= __qumir_debug_frames frames)
      (= __qumir_debug_capacity (* __qumir_debug_capacity 2))))

  (fun __qumir_debug_should_pause ((var kind i64) (var descriptor i64) (var frame_id i64)) -> bool
    (block
      (if (== __qumir_debug_mode_state 4) (return #t))
      (if (== kind 4)
        (block
          (if (!= (index __qumir_debug_points (+ descriptor 4)) 0) (return #t))
          (if (== __qumir_debug_mode_state 1) (return #t))
          (if (&& (== __qumir_debug_mode_state 2) (== frame_id __qumir_debug_target)) (return #t))))
      (return (&& (&& (> __qumir_debug_target 0) (< frame_id __qumir_debug_target))
        (|| (== __qumir_debug_mode_state 2) (== __qumir_debug_mode_state 3))))))

  ; A <future void> return type on a function with a body triggers coroutine lowering.
  ; Return a raw Future handle (0 for no-op) to avoid a coroutine frame at every point;
  ; the debug IR pass inserts await in callers, which handles both 0 and a JS Future.
  (fun __qumir_debug_point ((var id i32) (var address <ptr void>)) -> <ptr void>
    (block
      (if (== __qumir_debug_enabled 0) (return (cast 0 <ptr void>)))
      (if (== __qumir_debug_mode_state 5)
        (return (call __qumir_debug_slow id address (cast 2 i32))))
      (var descriptor = (* (cast id i64) 5))
      (var kind = (index __qumir_debug_points descriptor))
      (var function = (index __qumir_debug_points (+ descriptor 1)))
      (var pause = #f)
      (if (== kind 1)
        (block
          (if (== __qumir_debug_depth_state __qumir_debug_capacity) (call __qumir_debug_grow))
          (var frame = (* __qumir_debug_depth_state __qumir_debug_stride_state))
          (= __qumir_debug_next_frame (+ __qumir_debug_next_frame 1))
          (= __qumir_debug_frames [frame] __qumir_debug_next_frame)
          (= __qumir_debug_frames [(+ frame 1)] function)
          (= __qumir_debug_frames [(+ frame 2)] (cast id i64))
          (var i = 3)
          (while (< i __qumir_debug_stride_state)
            (block
              (= __qumir_debug_frames [(+ frame i)] 0)
              (= i (+ i 1))))
          (= __qumir_debug_depth_state (+ __qumir_debug_depth_state 1)))
        (if (> __qumir_debug_depth_state 0)
          (block
            (var frame = (* (- __qumir_debug_depth_state 1) __qumir_debug_stride_state))
            (if (== (index __qumir_debug_frames (+ frame 1)) function)
              (if (== kind 2)
                (= __qumir_debug_depth_state (- __qumir_debug_depth_state 1))
                (if (== kind 3)
                  (block
                    (var local = (index __qumir_debug_points (+ descriptor 3)))
                    (= __qumir_debug_frames [(+ (+ frame 3) local)] (cast address i64)))
                  (block
                    (= __qumir_debug_frames [(+ frame 2)] (cast id i64))
                    (var frame_id = (index __qumir_debug_frames frame))
                    (= pause (call __qumir_debug_should_pause kind descriptor frame_id)))))))))
      (if pause (return (call __qumir_debug_slow id address (cast 1 i32))))
      (return (cast 0 <ptr void>)))))
