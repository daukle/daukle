daukle.plugin{ api = 2, uses = { "exec" } }

daukle.toolchain{
  name = "t",
  generate = function(toolchain)
    return {}
  end,
}

daukle.source{
  name = "s",
  load = function(project, block, base_dir) end,
}
