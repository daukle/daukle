daukle.plugin{ api = 1, uses = {} }

daukle.language{
  name = "not-a-resolver",
  apply = function(consumer, resolved, text)
    return text
  end,
}
