define i32 @main() {
entry:
  %p = alloca i32
  store i32 7, i32* %p
  %v = load i32, i32* %p
  ret i32 %v
}
