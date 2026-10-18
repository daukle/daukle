daukle.plugin{ api = 3, uses = { "exec" } }

daukle.toolchain{ name = "stub", generate = function() return {} end }

daukle.translator{
  tool = "faketool",
  id = "org.example.release",
  translate = function(foreign)
    daukle.exec()
    return { manifest = {}, blockers = {} }
  end,
}
