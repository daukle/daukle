daukle.plugin{ api = 3, uses = { "exec" } }

daukle.migrator{
  tool = "faketool",
  toolchain = "stub",
  run = function(context)
    return { manifest = {}, foreign = {}, blockers = {} }
  end,
}
