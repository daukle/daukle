daukle.plugin{ api = 1, uses = { "provision" } }

daukle.provision{
  url    = "http://127.0.0.1:1/never-asked.tar.gz",
  sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
}

daukle.language{
  name = "early",
  apply = function(consumer, resolved, text) return text end,
}
