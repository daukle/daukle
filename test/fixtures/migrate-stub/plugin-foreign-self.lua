daukle.plugin{ api = 3, uses = { "write" } }

daukle.toolchain{ name = "stub", generate = function() return {} end }

daukle.task{
  name = "stub:build",
  run = function()
    daukle.write{ path = "greeting.txt", text = "hello" }
  end,
}

daukle.migrator{
  tool = "faketool",
  toolchain = "stub",
  run = function(context)
    return {
      manifest = { toolchains = { [context.instance] = {} } },
      foreign = { { kind = "plugins", id = "org.example.release", config = { draft = true } } },
      blockers = {},
      owned = { "faketool.conf", "out" },
      proof = {
        task = context.instance .. ":build",
        compare = { { old = "out/greeting.txt", new = "build/daukle/" .. context.instance .. "/greeting.txt", as = "bytes" } },
      },
    }
  end,
}

daukle.translator{
  tool = "faketool",
  id = "org.example.release",
  translate = function(foreign)
    return { manifest = { publish = { release = { from = foreign.instance } } }, blockers = {} }
  end,
}
