import vapoursynth as vs

core = vs.core
print("Read  :", core.imwri.Read.signature, "->", core.imwri.Read.return_signature)
print("Write :", core.imwri.Write.signature, "->", core.imwri.Write.return_signature)
