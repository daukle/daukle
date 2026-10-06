daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "secondpass",
  generate = function() return {} end,
}

daukle.task{
  name = "secondpass:greet",
  run = function() error("the staged working tree ran") end,
}
