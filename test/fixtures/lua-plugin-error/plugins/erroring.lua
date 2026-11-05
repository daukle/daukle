daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "erroring",
  generate = function(toolchain)
    error("boom from a toolchain plugin")
  end,
}
