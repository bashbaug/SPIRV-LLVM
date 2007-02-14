//===-- TargetData.cpp - Data size & alignment routines --------------------==//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines target properties related to datatype size/offset/alignment
// information.
//
// This structure should be created once, filled in if the defaults are not
// correct and then passed around by const&.  None of the members functions
// require modification to the object.
//
//===----------------------------------------------------------------------===//

#include "llvm/Target/TargetData.h"
#include "llvm/Module.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Constants.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringExtras.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>
using namespace llvm;

// Handle the Pass registration stuff necessary to use TargetData's.
namespace {
  // Register the default SparcV9 implementation...
  RegisterPass<TargetData> X("targetdata", "Target Data Layout");
}

//===----------------------------------------------------------------------===//
// Support for StructLayout
//===----------------------------------------------------------------------===//

StructLayout::StructLayout(const StructType *ST, const TargetData &TD) {
  StructAlignment = 0;
  StructSize = 0;
  NumElements = ST->getNumElements();

  // Loop over each of the elements, placing them in memory...
  for (unsigned i = 0, e = NumElements; i != e; ++i) {
    const Type *Ty = ST->getElementType(i);
    unsigned TyAlign;
    uint64_t TySize;
    TyAlign = (unsigned) TD.getABITypeAlignment(Ty);
    TySize = (unsigned) TD.getTypeSize(Ty);

    // Add padding if necessary to make the data element aligned properly...
    if (StructSize % TyAlign != 0)
      StructSize = (StructSize/TyAlign + 1) * TyAlign;   // Add padding...

    // Keep track of maximum alignment constraint
    StructAlignment = std::max(TyAlign, StructAlignment);

    MemberOffsets[i] = StructSize;
    StructSize += TySize;                 // Consume space for this data item
  }

  // Empty structures have alignment of 1 byte.
  if (StructAlignment == 0) StructAlignment = 1;

  // Add padding to the end of the struct so that it could be put in an array
  // and all array elements would be aligned correctly.
  if (StructSize % StructAlignment != 0)
    StructSize = (StructSize/StructAlignment + 1) * StructAlignment;
}


/// getElementContainingOffset - Given a valid offset into the structure,
/// return the structure index that contains it.
unsigned StructLayout::getElementContainingOffset(uint64_t Offset) const {
  const uint64_t *SI =
    std::upper_bound(&MemberOffsets[0], &MemberOffsets[NumElements], Offset);
  assert(SI != &MemberOffsets[0] && "Offset not in structure type!");
  --SI;
  assert(*SI <= Offset && "upper_bound didn't work");
  assert((SI == &MemberOffsets[0] || *(SI-1) < Offset) &&
         (SI+1 == &MemberOffsets[NumElements] || *(SI+1) > Offset) &&
         "Upper bound didn't work!");
  return SI-&MemberOffsets[0];
}

//===----------------------------------------------------------------------===//
// TargetAlignElem, TargetAlign support
//===----------------------------------------------------------------------===//

TargetAlignElem
TargetAlignElem::get(AlignTypeEnum align_type, unsigned char abi_align,
                     unsigned char pref_align, short bit_width)
{
  TargetAlignElem retval;
  retval.AlignType = align_type;
  retval.ABIAlign = abi_align;
  retval.PrefAlign = pref_align;
  retval.TypeBitWidth = bit_width;
  return retval;
}

bool
TargetAlignElem::operator<(const TargetAlignElem &rhs) const
{
  return ((AlignType < rhs.AlignType)
          || (AlignType == rhs.AlignType && TypeBitWidth < rhs.TypeBitWidth));
}

bool
TargetAlignElem::operator==(const TargetAlignElem &rhs) const
{
  return (AlignType == rhs.AlignType
          && ABIAlign == rhs.ABIAlign
          && PrefAlign == rhs.PrefAlign
          && TypeBitWidth == rhs.TypeBitWidth);
}

std::ostream &
TargetAlignElem::dump(std::ostream &os) const
{
  return os << AlignType
            << TypeBitWidth
            << ":" << (int) (ABIAlign * 8)
            << ":" << (int) (PrefAlign * 8);
}

std::ostream &
llvm::operator<<(std::ostream &os, const TargetAlignElem &elem)
{
  return elem.dump(os);
}

const TargetAlignElem TargetData::InvalidAlignmentElem =
                TargetAlignElem::get((AlignTypeEnum) -1, 0, 0, 0);

//===----------------------------------------------------------------------===//
//                       TargetData Class Implementation
//===----------------------------------------------------------------------===//

/*!
 A TargetDescription string consists of a sequence of hyphen-delimited
 specifiers for target endianness, pointer size and alignments, and various
 primitive type sizes and alignments. A typical string looks something like:
 <br>
 "E-p:32:32:32-i1:8:8-i8:8:8-i32:32:32-i64:32:64-f32:32:32-f64:32:64"
 <br>
 (note: this string is not fully specified and is only an example.)
 \p
 Alignments come in two flavors: ABI and preferred. ABI alignment (abi_align,
 below) dictates how a type will be aligned within an aggregate and when used
 as an argument.  Preferred alignment (pref_align, below) determines a type's
 alignment when emitted as a global.
 \p
 Specifier string details:
 <br><br>
 <i>[E|e]</i>: Endianness. "E" specifies a big-endian target data model, "e"
 specifies a little-endian target data model.
 <br><br>
 <i>p:<size>:<abi_align>:<pref_align></i>: Pointer size, ABI and preferred
 alignment.
 <br><br>
 <i><type><size>:<abi_align>:<pref_align></i>: Numeric type alignment. Type is
 one of <i>i|f|v|a</i>, corresponding to integer, floating point, vector (aka
 packed) or aggregate.  Size indicates the size, e.g., 32 or 64 bits.
 \p
 The default string, fully specified is:
 <br><br>
 "E-p:64:64:64-a0:0:0-f32:32:32-f64:0:64"
 "-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:0:64"
 "-v64:64:64-v128:128:128"
 <br><br>
 Note that in the case of aggregates, 0 is the default ABI and preferred
 alignment. This is a special case, where the aggregate's computed worst-case
 alignment will be used.
 */ 
void TargetData::init(const std::string &TargetDescription) {
  std::string temp = TargetDescription;
  
  LittleEndian = false;
  PointerMemSize = 8;
  PointerABIAlign   = 8;
  PointerPrefAlign = PointerABIAlign;

  // Default alignments
  setAlignment(INTEGER_ALIGN,   1,  1, 1); // Bool
  setAlignment(INTEGER_ALIGN,   1,  1, 8); // Byte
  setAlignment(INTEGER_ALIGN,   2,  2, 16); // short
  setAlignment(INTEGER_ALIGN,   4,  4, 32); // int
  setAlignment(INTEGER_ALIGN,   0,  8, 64); // long
  setAlignment(FLOAT_ALIGN,     4,  4, 32); // float
  setAlignment(FLOAT_ALIGN,     0,  8, 64); // double
  setAlignment(PACKED_ALIGN,    8,  8, 64); // v2i32
  setAlignment(PACKED_ALIGN,   16, 16, 128); // v16i8, v8i16, v4i32, ...
  setAlignment(AGGREGATE_ALIGN, 0,  0,  0); // struct, union, class, ...
  
  while (!temp.empty()) {
    std::string token = getToken(temp, "-");
    
    std::string arg0 = getToken(token, ":");
    const char *p = arg0.c_str();
    AlignTypeEnum align_type;
    short size;
    unsigned char abi_align;
    unsigned char pref_align;

    switch(*p) {
    case 'E':
      LittleEndian = false;
      break;
    case 'e':
      LittleEndian = true;
      break;
    case 'p':
      PointerMemSize = atoi(getToken(token,":").c_str()) / 8;
      PointerABIAlign = atoi(getToken(token,":").c_str()) / 8;
      PointerPrefAlign = atoi(getToken(token,":").c_str()) / 8;
      if (PointerPrefAlign == 0)
        PointerPrefAlign = PointerABIAlign;
      break;
    case 'i':
    case 'v':
    case 'f':
    case 'a': {
      align_type = (*p == 'i' ? INTEGER_ALIGN :
                    (*p == 'f' ? FLOAT_ALIGN :
                     (*p == 'v' ? PACKED_ALIGN : AGGREGATE_ALIGN)));
      size = (short) atoi(++p);
      abi_align = atoi(getToken(token, ":").c_str()) / 8;
      pref_align = atoi(getToken(token, ":").c_str()) / 8;
      if (pref_align == 0)
        pref_align = abi_align;
      setAlignment(align_type, abi_align, pref_align, size);
      break;
    }
    default:
      break;
    }
  }

  // Unless explicitly specified, the alignments for longs and doubles is 
  // capped by pointer size.
  // FIXME: Is this still necessary?
  const TargetAlignElem &long_align = getAlignment(INTEGER_ALIGN, 64);
  if (long_align.ABIAlign == 0)
    setAlignment(INTEGER_ALIGN, PointerMemSize, PointerMemSize, 64);

  const TargetAlignElem &double_align = getAlignment(FLOAT_ALIGN, 64);
  if (double_align.ABIAlign == 0)
    setAlignment(FLOAT_ALIGN, PointerMemSize, PointerMemSize, 64);
}

TargetData::TargetData(const Module *M) {
  init(M->getDataLayout());
}

void
TargetData::setAlignment(AlignTypeEnum align_type, unsigned char abi_align,
                         unsigned char pref_align, short bit_width) {
  TargetAlignElem elt = TargetAlignElem::get(align_type, abi_align,
                                             pref_align, bit_width);
  std::pair<align_iterator, align_iterator> ins_result =
            std::equal_range(Alignments.begin(), Alignments.end(), elt);
  align_iterator I = ins_result.first;
  if (I->AlignType == align_type && I->TypeBitWidth == bit_width) {
    // Update the abi, preferred alignments.
    I->ABIAlign = abi_align;
    I->PrefAlign = pref_align;
  } else
    Alignments.insert(I, elt);

#if 0
  // Keep around for debugging and testing...
  align_iterator E = ins_result.second;

  cerr << "setAlignment(" << elt << ")\n";
  cerr << "I = " << (I - Alignments.begin())
       << ", E = " << (E - Alignments.begin()) << "\n";
  std::copy(Alignments.begin(), Alignments.end(),
            std::ostream_iterator<TargetAlignElem>(*cerr, "\n"));
  cerr << "=====\n";
#endif
}

const TargetAlignElem &
TargetData::getAlignment(AlignTypeEnum align_type, short bit_width) const
{
  std::pair<align_const_iterator, align_const_iterator> find_result =
                std::equal_range(Alignments.begin(), Alignments.end(),
                                 TargetAlignElem::get(align_type, 0, 0,
                                                      bit_width));
  align_const_iterator I = find_result.first;

  // Note: This may not be reasonable if variable-width integer sizes are
  // passed, at which point, more sophisticated searching will need to be done.
  return *I;
}

/// LayoutInfo - The lazy cache of structure layout information maintained by
/// TargetData.  Note that the struct types must have been free'd before
/// llvm_shutdown is called (and thus this is deallocated) because all the
/// targets with cached elements should have been destroyed.
///
typedef std::pair<const TargetData*,const StructType*> LayoutKey;

struct DenseMapLayoutKeyInfo {
  static inline LayoutKey getEmptyKey() { return LayoutKey(0, 0); }
  static inline LayoutKey getTombstoneKey() {
    return LayoutKey((TargetData*)(intptr_t)-1, 0);
  }
  static unsigned getHashValue(const LayoutKey &Val) {
    return DenseMapKeyInfo<void*>::getHashValue(Val.first) ^
           DenseMapKeyInfo<void*>::getHashValue(Val.second);
  }
  static bool isPod() { return true; }
};

typedef DenseMap<LayoutKey, StructLayout*, DenseMapLayoutKeyInfo> LayoutInfoTy;
static ManagedStatic<LayoutInfoTy> LayoutInfo;


TargetData::~TargetData() {
  if (LayoutInfo.isConstructed()) {
    // Remove any layouts for this TD.
    LayoutInfoTy &TheMap = *LayoutInfo;
    for (LayoutInfoTy::iterator I = TheMap.begin(), E = TheMap.end();
         I != E; ) {
      if (I->first.first == this) {
        I->second->~StructLayout();
        free(I->second);
        TheMap.erase(I++);
      } else {
        ++I;
      }
    }
  }
}

const StructLayout *TargetData::getStructLayout(const StructType *Ty) const {
  LayoutInfoTy &TheMap = *LayoutInfo;
  
  StructLayout *&SL = TheMap[LayoutKey(this, Ty)];
  if (SL) return SL;

  // Otherwise, create the struct layout.  Because it is variable length, we 
  // malloc it, then use placement new.
  unsigned NumElts = Ty->getNumElements();
  StructLayout *L =
    (StructLayout *)malloc(sizeof(StructLayout)+(NumElts-1)*sizeof(uint64_t));
  
  // Set SL before calling StructLayout's ctor.  The ctor could cause other
  // entries to be added to TheMap, invalidating our reference.
  SL = L;
  
  new (L) StructLayout(Ty, *this);
    
  return L;
}

/// InvalidateStructLayoutInfo - TargetData speculatively caches StructLayout
/// objects.  If a TargetData object is alive when types are being refined and
/// removed, this method must be called whenever a StructType is removed to
/// avoid a dangling pointer in this cache.
void TargetData::InvalidateStructLayoutInfo(const StructType *Ty) const {
  if (!LayoutInfo.isConstructed()) return;  // No cache.
  
  LayoutInfoTy::iterator I = LayoutInfo->find(LayoutKey(this, Ty));
  if (I != LayoutInfo->end()) {
    I->second->~StructLayout();
    free(I->second);
    LayoutInfo->erase(I);
  }
}


struct hyphen_delimited :
  public std::iterator<std::output_iterator_tag, void, void, void, void>
{
  std::ostream &o;

  hyphen_delimited(std::ostream &os) :
    o(os)
  { }

  hyphen_delimited &operator=(const TargetAlignElem &elem)
  {
    o << "-" << elem;
    return *this;
  }

  hyphen_delimited &operator*()
  {
    return *this;
  }

  hyphen_delimited &operator++()
  {
    return *this;
  }
};


std::string TargetData::getStringRepresentation() const {
  std::stringstream repr;

  if (LittleEndian)
    repr << "e";
  else
    repr << "E";
  repr << "-p:" << (PointerMemSize * 8) << ":" << (PointerABIAlign * 8)
       << ":" << (PointerPrefAlign * 8);
  std::copy(Alignments.begin(), Alignments.end(), hyphen_delimited(repr));
  return repr.str();
}


uint64_t TargetData::getTypeSize(const Type *Ty) const {
  assert(Ty->isSized() && "Cannot getTypeInfo() on a type that is unsized!");
  switch (Ty->getTypeID()) {
  case Type::LabelTyID:
  case Type::PointerTyID:
    return getPointerSize();
  case Type::ArrayTyID: {
    const ArrayType *ATy = cast<ArrayType>(Ty);
    uint64_t Size;
    unsigned char Alignment;
    Size = getTypeSize(ATy->getElementType());
    Alignment = getABITypeAlignment(ATy->getElementType());
    unsigned AlignedSize = (Size + Alignment - 1)/Alignment*Alignment;
    return AlignedSize*ATy->getNumElements();
  }
  case Type::StructTyID: {
    // Get the layout annotation... which is lazily created on demand.
    const StructLayout *Layout = getStructLayout(cast<StructType>(Ty));
    return Layout->getSizeInBytes();
  }
  case Type::IntegerTyID: {
    unsigned BitWidth = cast<IntegerType>(Ty)->getBitWidth();
    if (BitWidth <= 8) {
      return 1;
    } else if (BitWidth <= 16) {
      return 2;
    } else if (BitWidth <= 32) {
      return 4;
    } else if (BitWidth <= 64) {
      return 8;
    } else
      assert(0 && "Integer types > 64 bits not supported.");
    break;
  }
  case Type::VoidTyID:
    return 1;
  case Type::FloatTyID:
    return 4;
  case Type::DoubleTyID:
    return 8;
  case Type::PackedTyID: {
    const PackedType *PTy = cast<PackedType>(Ty);
    return PTy->getBitWidth() / 8;
  }
  default:
    assert(0 && "TargetData::getTypeSize(): Unsupported type");
    break;
  }
  return 0;
}

uint64_t TargetData::getTypeSizeInBits(const Type *Ty) const {
  if (Ty->isInteger())
    return cast<IntegerType>(Ty)->getBitWidth();
  else
    return getTypeSize(Ty) * 8;
}


/*!
  \param abi_or_pref Flag that determines which alignment is returned. true
  returns the ABI alignment, false returns the preferred alignment.
  \param Ty The underlying type for which alignment is determined.

  Get the ABI (\a abi_or_pref == true) or preferred alignment (\a abi_or_pref
  == false) for the requested type \a Ty.
 */
unsigned char TargetData::getAlignment(const Type *Ty, bool abi_or_pref) const
{
  int AlignType = -1;

  assert(Ty->isSized() && "Cannot getTypeInfo() on a type that is unsized!");
  switch (Ty->getTypeID()) {
  /* Early escape for the non-numeric types */
  case Type::LabelTyID:
  case Type::PointerTyID:
    return (abi_or_pref
            ? getPointerABIAlignment()
            : getPointerPrefAlignment());
  case Type::ArrayTyID: {
    const ArrayType *ATy = cast<ArrayType>(Ty);
    return (abi_or_pref
            ? getABITypeAlignment(ATy->getElementType())
            : getPrefTypeAlignment(ATy->getElementType()));
  }
  case Type::StructTyID: {
      // Get the layout annotation... which is lazily created on demand.
    const StructLayout *Layout = getStructLayout(cast<StructType>(Ty));
    const TargetAlignElem &elem = getAlignment(AGGREGATE_ALIGN, 0);
    assert(validAlignment(elem)
           && "Aggregate alignment return invalid in getAlignment");
    if (abi_or_pref) {
      return (elem.ABIAlign < Layout->getAlignment()
              ? Layout->StructAlignment
              : elem.ABIAlign);
    } else {
      return (elem.PrefAlign < Layout->getAlignment()
              ? Layout->StructAlignment
              : elem.PrefAlign);
    }
  }
  case Type::IntegerTyID:
  case Type::VoidTyID:
    AlignType = INTEGER_ALIGN;
    break;
  case Type::FloatTyID:
  case Type::DoubleTyID:
    AlignType = FLOAT_ALIGN;
    break;
  case Type::PackedTyID:
    AlignType = PACKED_ALIGN;
    break;
  default:
    assert(0 && "Bad type for getAlignment!!!");
    break;
  }

  const TargetAlignElem &elem = getAlignment((AlignTypeEnum) AlignType,
                                             getTypeSize(Ty) * 8);
  if (validAlignment(elem))
    return (abi_or_pref ? elem.ABIAlign : elem.PrefAlign);
  else {
    cerr << "TargetData::getAlignment: align type " << AlignType
         << " size " << getTypeSize(Ty) << " not found in Alignments.\n";
    abort();
    /*NOTREACHED*/
    return 0;
  }
}

unsigned char TargetData::getABITypeAlignment(const Type *Ty) const {
  return getAlignment(Ty, true);
}

unsigned char TargetData::getPrefTypeAlignment(const Type *Ty) const {
  return getAlignment(Ty, false);
}

unsigned char TargetData::getPreferredTypeAlignmentShift(const Type *Ty) const {
  unsigned Align = (unsigned) getPrefTypeAlignment(Ty);
  assert(!(Align & (Align-1)) && "Alignment is not a power of two!");
  return Log2_32(Align);
}

/// getIntPtrType - Return an unsigned integer type that is the same size or
/// greater to the host pointer size.
const Type *TargetData::getIntPtrType() const {
  switch (getPointerSize()) {
  default: assert(0 && "Unknown pointer size!");
  case 2: return Type::Int16Ty;
  case 4: return Type::Int32Ty;
  case 8: return Type::Int64Ty;
  }
}


uint64_t TargetData::getIndexedOffset(const Type *ptrTy, Value* const* Indices,
                                      unsigned NumIndices) const {
  const Type *Ty = ptrTy;
  assert(isa<PointerType>(Ty) && "Illegal argument for getIndexedOffset()");
  uint64_t Result = 0;

  generic_gep_type_iterator<Value* const*>
    TI = gep_type_begin(ptrTy, Indices, Indices+NumIndices);
  for (unsigned CurIDX = 0; CurIDX != NumIndices; ++CurIDX, ++TI) {
    if (const StructType *STy = dyn_cast<StructType>(*TI)) {
      assert(Indices[CurIDX]->getType() == Type::Int32Ty &&"Illegal struct idx");
      unsigned FieldNo = cast<ConstantInt>(Indices[CurIDX])->getZExtValue();

      // Get structure layout information...
      const StructLayout *Layout = getStructLayout(STy);

      // Add in the offset, as calculated by the structure layout info...
      Result += Layout->getElementOffset(FieldNo);

      // Update Ty to refer to current element
      Ty = STy->getElementType(FieldNo);
    } else {
      // Update Ty to refer to current element
      Ty = cast<SequentialType>(Ty)->getElementType();

      // Get the array index and the size of each array element.
      int64_t arrayIdx = cast<ConstantInt>(Indices[CurIDX])->getSExtValue();
      Result += arrayIdx * (int64_t)getTypeSize(Ty);
    }
  }

  return Result;
}

/// getPreferredAlignmentLog - Return the preferred alignment of the
/// specified global, returned in log form.  This includes an explicitly
/// requested alignment (if the global has one).
unsigned TargetData::getPreferredAlignmentLog(const GlobalVariable *GV) const {
  const Type *ElemType = GV->getType()->getElementType();
  unsigned Alignment = getPreferredTypeAlignmentShift(ElemType);
  if (GV->getAlignment() > (1U << Alignment))
    Alignment = Log2_32(GV->getAlignment());
  
  if (GV->hasInitializer()) {
    if (Alignment < 4) {
      // If the global is not external, see if it is large.  If so, give it a
      // larger alignment.
      if (getTypeSize(ElemType) > 128)
        Alignment = 4;    // 16-byte alignment.
    }
  }
  return Alignment;
}
