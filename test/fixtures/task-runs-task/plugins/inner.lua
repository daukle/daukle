daukle.plugin{ api = 1, uses = { "write" } }

daukle.toolchain{
  name = "inner",
  generate = function() return {} end,
}

daukle.task{
  name = "inner:mark",
  run = function() daukle.write{ path = "inner.txt", text = "inner" } end,
}

daukle.task{
  name = "inner:fail",
  run = function() error("the inner task said no", 0) end,
}
