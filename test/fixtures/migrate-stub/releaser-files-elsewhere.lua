daukle.plugin{ api = 8 }

daukle.translator{
  tool = "faketool",
  id = "org.example.release",
  translate = function()
    return { manifest = {}, blockers = {}, files = { { path = "daukle/stub/pins.txt", text = "pinned" } } }
  end,
}
