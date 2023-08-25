set(LLVM_LINK_COMPONENTS
  BinaryFormat
  BitReader
  Core
  Demangle
  Remarks
  Support
  TargetParser
  )

add_llvm_tool(llvm-html
  HTMLAsmWriter.cpp
  llvm-html.cpp

  DEPENDS
  intrinsics_gen
  )
