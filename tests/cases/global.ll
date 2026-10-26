@g = global i32 41

define i32 @main() {
entry:
  %v = load i32, ptr @g
  %r = add i32 %v, 1
  ret i32 %r
}
