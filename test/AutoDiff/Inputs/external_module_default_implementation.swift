public protocol P: Differentiable {
  @differentiable
  @differentiable(wrt: self)
  func f(_ x: Float) -> Float
}

public extension P {
  @differentiable
  func f(_ x: Float) -> Float { x * x }
}
