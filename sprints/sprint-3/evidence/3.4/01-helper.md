# US-3.4 evidence — hu_embed_helper + embed-existing-memories.sh

## AC-3.4.1 — helper compiles & links
```
-rwxr-xr-x@ 1 sethford  staff  76720 May 15 10:26 build/hu_embed_helper
```

## AC-3.4.2 — helper accepts text via argv, outputs one-line JSON
```
{"embedder":"tfidf-local-v1","dimensions":384,"text_len":11,"embedding":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
```

## AC-3.4.3 — helper accepts text via stdin
```
{"embedder":"tfidf-local-v1","dimensions":384,"text_len":44,"embedding":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0.3015113,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0.6030227,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0.3015113,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
```

## JSON validates as parseable + non-zero embedding
```
embedder: tfidf-local-v1
dimensions: 384
text_len: 32
nonzero values: 3
first 5: [0, 0, 0, 0, 0]
```
