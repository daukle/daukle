daukle.plugin{ api = 3, uses = { "exec" } }

daukle.toolchain{ name = "stub", generate = function() return {} end }

daukle.migrator{
  tool = "faketool",
  toolchain = "stub",
  run = function(context)
    daukle.exec()
    return {}
  end,
}
