daukle.plugin{ api = 3, uses = { "write" } }

daukle.toolchain{ name = "stub", generate = function() return {} end }

daukle.migrator{
  tool = "faketool",
  toolchain = "stub",
  run = function(context)
    return { manifest = {}, foreign = {}, blockers = {} }
  end,
}
