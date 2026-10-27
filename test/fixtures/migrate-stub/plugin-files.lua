daukle.plugin{ api = 4, uses = { "write", "read" } }

daukle.toolchain{ name = "stub", generate = function() return {} end }

daukle.task{
  name = "stub:build",
  run = function()
    daukle.write{ path = "greeting.txt", text = daukle.read("daukle/stub/carried.txt") }
  end,
}

daukle.migrator{
  tool = "faketool",
  toolchain = "stub",
  run = function(context)
    return {
      manifest = { toolchains = { [context.instance] = {} } },
      foreign = {},
      blockers = {},
      files = { { path = "daukle/" .. context.instance .. "/carried.txt", text = "hello" } },
      owned = { "faketool.conf", "out" },
      proof = {
        task = context.instance .. ":build",
        compare = { { old = "out/greeting.txt", new = "build/daukle/" .. context.instance .. "/greeting.txt", as = "bytes" } },
      },
    }
  end,
}
