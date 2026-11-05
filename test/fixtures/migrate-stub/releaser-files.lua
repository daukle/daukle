daukle.plugin{ api = 8 }

daukle.translator{
  tool = "faketool",
  id = "org.example.release",
  translate = function(foreign)
    return {
      manifest = { publish = { release = { from = foreign.instance } } },
      blockers = {},
      files = { { path = "daukle/releaser/pins.txt", text = "pinned" } },
    }
  end,
}

daukle.publisher{ name = "release", publish = function() end }
