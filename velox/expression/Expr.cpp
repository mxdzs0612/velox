/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/expression/Expr.h"
#include "velox/core/Expressions.h"
#include "velox/expression/ControlExpr.h"
#include "velox/expression/ExprCompiler.h"
#include "velox/expression/VarSetter.h"
#include "velox/expression/VectorFunction.h"

namespace facebook::velox::exec {

using functions::stringCore::maxEncoding;
using functions::stringCore::StringEncodingMode;

namespace {

bool isMember(
    const std::vector<FieldReference*>& fields,
    FieldReference* field) {
  return std::find(fields.begin(), fields.end(), field) != fields.end();
}

void mergeFields(
    std::vector<FieldReference*>& fields,
    const std::vector<FieldReference*>& moreFields) {
  for (auto* newField : moreFields) {
    if (!isMember(fields, newField)) {
      fields.emplace_back(newField);
    }
  }
}

// Returns true if input expression or any sub-expression is an IF, AND or OR.
bool hasConditionals(Expr* expr) {
  if (expr->isConditional()) {
    return true;
  }

  for (const auto& child : expr->inputs()) {
    if (hasConditionals(child.get())) {
      return true;
    }
  }

  return false;
}
} // namespace

// static
bool Expr::isSameFields(
    const std::vector<FieldReference*>& fields1,
    const std::vector<FieldReference*>& fields2) {
  if (fields1.size() != fields2.size()) {
    return false;
  }
  return std::all_of(
      fields1.begin(), fields1.end(), [&fields2](const auto& field) {
        return isMember(fields2, field);
      });
}

bool Expr::isSubsetOfFields(
    const std::vector<FieldReference*>& subset,
    const std::vector<FieldReference*>& superset) {
  if (subset.size() > superset.size()) {
    return false;
  }
  return std::all_of(
      subset.begin(), subset.end(), [&superset](const auto& field) {
        return isMember(superset, field);
      });
}

void Expr::computeMetadata() {
  // Sets propagatesNulls if all subtrees propagate nulls.
  // Sets isDeterministic to false if some subtree is non-deterministic.
  // Sets 'distinctFields_' to be the union of 'distinctFields_' of inputs.
  // If one of the inputs has the identical set of distinct fields, then
  // the input's distinct fields are set to empty.
  if (isSpecialForm()) {
    // 'propagatesNulls_' will be adjusted after inputs are processed.
    propagatesNulls_ = true;
    deterministic_ = true;
  } else if (vectorFunction_) {
    propagatesNulls_ = vectorFunction_->isDefaultNullBehavior();
    deterministic_ = vectorFunction_->isDeterministic();
  }
  for (auto& input : inputs_) {
    input->computeMetadata();
    deterministic_ &= input->deterministic_;
    propagatesNulls_ &= input->propagatesNulls_;
    mergeFields(distinctFields_, input->distinctFields_);
  }
  if (isSpecialForm()) {
    propagatesNulls_ = propagatesNulls();
  }
  for (auto& input : inputs_) {
    if (!input->isMultiplyReferenced_ &&
        isSameFields(distinctFields_, input->distinctFields_)) {
      input->distinctFields_.clear();
    }
  }

  hasConditionals_ = hasConditionals(this);
}

namespace {
// Returns true if vector is a LazyVector that hasn't been loaded yet or
// is not dictionary, sequence or constant encoded.
bool isFlat(const BaseVector& vector) {
  auto encoding = vector.encoding();
  if (encoding == VectorEncoding::Simple::LAZY) {
    if (!vector.asUnchecked<LazyVector>()->isLoaded()) {
      return true;
    }

    encoding = vector.loadedVector()->encoding();
  }
  return !(
      encoding == VectorEncoding::Simple::SEQUENCE ||
      encoding == VectorEncoding::Simple::DICTIONARY ||
      encoding == VectorEncoding::Simple::CONSTANT);
}
} // namespace

void Expr::eval(
    const SelectivityVector& rows,
    EvalCtx* context,
    VectorPtr* result) {
  if (!rows.hasSelections()) {
    return;
  }

  // Check if there are any IFs, ANDs or ORs. These expressions are special
  // because not all of their sub-expressions get evaluated on all the rows all
  // the time. Therefore, we should delay loading lazy vectors until we know the
  // minimum subset of rows needed to be loaded.
  //
  // If there is only one field, load it unconditionally. The very first IF, AND
  // or OR will have to load it anyway. Pre-loading enables peeling of encodings
  // at a higher level in the expression tree and avoids repeated peeling and
  // wrapping in the sub-nodes.
  //
  // TODO: Re-work the logic of deciding when to load which field.
  if (!hasConditionals_ || distinctFields_.size() == 1) {
    // Load lazy vectors if any.
    for (const auto& field : distinctFields_) {
      context->ensureFieldLoaded(field->index(context), rows);
    }
  }

  // Invalidate old result string encoding if exist
  if (isString() && result && *result) {
    (*result)
        ->asUnchecked<SimpleVector<StringView>>()
        ->invalidateStringEncoding();
  }
  if (inputs_.empty()) {
    evalAll(rows, context, result);
    return;
  }

  if (checkGetSharedSubexprValues(rows, context, result)) {
    return;
  }

  evalEncodings(rows, context, result);

  checkUpdateSharedSubexprValues(rows, context, *result);
}

bool Expr::checkGetSharedSubexprValues(
    const SelectivityVector& rows,
    EvalCtx* context,
    VectorPtr* result) {
  // Common subexpression optimization and peeling off of encodings and lazy
  // vectors do not work well together. There are cases when expression
  // initially is evaluated on rows before peeling and later is evaluated on
  // rows after peeling. In this case the row numbers in sharedSubexprRows_ are
  // not comparable to 'rows'.
  //
  // For now, disable the optimization if any encodings have been peeled off.

  if (!isMultiplyReferenced_ || !sharedSubexprValues_ ||
      context->wrapEncoding_ != VectorEncoding::Simple::FLAT) {
    return false;
  }

  if (!rows.isSubset(*sharedSubexprRows_)) {
    LocalSelectivityVector missingRowsHolder(context, rows);
    auto missingRows = missingRowsHolder.get();
    missingRows->deselect(*sharedSubexprRows_);

    // Fix finalSelection at "rows" if missingRows is a strict subset to avoid
    // losing values outside of missingRows.
    bool updateFinalSelection = context->isFinalSelection() &&
        (missingRows->countSelected() < rows.countSelected());
    bool newIsFinalSelection =
        updateFinalSelection ? false : context->isFinalSelection();
    VarSetter finalSelectionOr(
        context->mutableFinalSelection(), &rows, updateFinalSelection);
    VarSetter isFinalSelectionOr(
        context->mutableIsFinalSelection(), newIsFinalSelection);

    evalEncodings(*missingRows, context, &sharedSubexprValues_);
  }
  context->moveOrCopyResult(sharedSubexprValues_, rows, result);
  // Copy string encoding
  if (isString()) {
    (*result)->as<SimpleVector<StringView>>()->copyStringEncodingFrom(
        sharedSubexprValues_.get());
  }
  return true;
}

void Expr::checkUpdateSharedSubexprValues(
    const SelectivityVector& rows,
    EvalCtx* context,
    const VectorPtr& result) {
  if (!isMultiplyReferenced_ || sharedSubexprValues_ ||
      context->wrapEncoding_ != VectorEncoding::Simple::FLAT) {
    return;
  }

  if (!sharedSubexprRows_) {
    sharedSubexprRows_ = context->execCtx()->getSelectivityVector(rows.size());
  }
  *sharedSubexprRows_ = rows;
  sharedSubexprValues_ = result;
}

namespace {
inline void setPeeled(
    const VectorPtr& leaf,
    int32_t fieldIndex,
    EvalCtx* context,
    std::vector<VectorPtr>& peeled) {
  if (peeled.size() <= fieldIndex) {
    peeled.resize(context->row()->childrenSize());
  }
  assert(peeled.size() > fieldIndex);
  peeled[fieldIndex] = leaf;
}

/// Translates row number of the outer vector into row number of the inner
/// vector using DecodedVector.
SelectivityVector* translateToInnerRows(
    const SelectivityVector& rows,
    DecodedVector& decoded,
    LocalSelectivityVector& newRowsHolder) {
  auto baseSize = decoded.base()->size();
  auto indices = decoded.indices();
  // If the wrappers add nulls, do not enable the inner rows. The
  // indices for places that a dictionary sets to null are not
  // defined. Null adding dictionaries are not peeled off non
  // null-propagating Exprs.
  auto flatNulls = decoded.nullIndices() != indices ? decoded.nulls() : nullptr;

  auto newRows = newRowsHolder.get(baseSize);
  newRows->clearAll();
  rows.applyToSelected([&](vector_size_t row) {
    if (!flatNulls || !bits::isBitNull(flatNulls, row)) {
      newRows->setValid(indices[row], true);
    }
  });
  newRows->updateBounds();

  return newRows;
}

template <typename T, typename U>
BufferPtr newBuffer(const U* data, size_t size, memory::MemoryPool* pool) {
  BufferPtr buffer = AlignedBuffer::allocate<T>(size, pool);
  memcpy(buffer->asMutable<char>(), data, BaseVector::byteSize<T>(size));
  return buffer;
}

SelectivityVector* singleRow(
    LocalSelectivityVector& holder,
    vector_size_t row) {
  holder.allocate(row + 1);
  auto rows = holder.get();
  rows->clearAll();
  rows->setValid(row, true);
  rows->updateBounds();
  return rows;
}
} // namespace

void Expr::setDictionaryWrapping(
    DecodedVector& decoded,
    const SelectivityVector& rows,
    BaseVector& firstWrapper,
    EvalCtx* context) {
  context->wrapEncoding_ = VectorEncoding::Simple::DICTIONARY;
  if (decoded.indicesNotCopied() && decoded.nullsNotCopied()) {
    context->wrap_ = firstWrapper.wrapInfo();
    context->wrapNulls_ = firstWrapper.nulls();
  } else {
    context->wrap_ = newBuffer<vector_size_t>(
        decoded.indices(), rows.end(), context->execCtx()->pool());
    if (decoded.hasExtraNulls()) {
      // Nulls are added by wrapping. Add a null wrap.
      context->wrapNulls_ = newBuffer<bool>(
          decoded.nulls(), rows.end(), context->execCtx()->pool());
    }
  }
}

std::pair<SelectivityVector*, SelectivityVector*> Expr::peelEncodings(
    EvalCtx* context,
    ContextSaver* saver,
    const SelectivityVector& rows,
    LocalDecodedVector& localDecoded,
    LocalSelectivityVector& newRowsHolder,
    LocalSelectivityVector& finalRowsHolder) {
  if (context->wrapEncoding_ == VectorEncoding::Simple::CONSTANT) {
    return std::make_pair(nullptr, nullptr);
  }
  std::vector<VectorPtr> peeledVectors;
  std::vector<VectorPtr> maybePeeled;
  std::vector<bool> constantFields;
  int numLevels = 0;
  bool peeled;
  bool nonConstant = false;
  auto numFields = context->row()->childrenSize();
  int32_t firstPeeled = -1;
  do {
    peeled = true;
    BufferPtr firstIndices;
    BufferPtr firstLengths;
    for (const auto& field : distinctFields_) {
      auto fieldIndex = field->index(context);
      assert(fieldIndex >= 0 && fieldIndex < numFields);
      auto leaf = peeledVectors.empty() ? context->getField(fieldIndex)
                                        : peeledVectors[fieldIndex];
      if (!constantFields.empty() && constantFields[fieldIndex]) {
        setPeeled(leaf, fieldIndex, context, maybePeeled);
        continue;
      }
      if (numLevels == 0 && leaf->isConstant(rows)) {
        setPeeled(leaf, fieldIndex, context, maybePeeled);
        constantFields.resize(numFields);
        constantFields.at(fieldIndex) = true;
        continue;
      }
      nonConstant = true;
      auto encoding = leaf->encoding();
      if (encoding == VectorEncoding::Simple::DICTIONARY) {
        if (firstLengths) {
          // having a mix of dictionary and sequence encoded fields
          peeled = false;
          break;
        }
        if (!propagatesNulls_ && leaf->rawNulls()) {
          // A dictionary that adds nulls over an Expr that is not null for a
          // null argument cannot be peeled.
          peeled = false;
          break;
        }
        BufferPtr indices = leaf->wrapInfo();
        if (!firstIndices) {
          firstIndices = std::move(indices);
        } else if (indices != firstIndices) {
          // different fields use different dictionaries
          peeled = false;
          break;
        }
        if (firstPeeled == -1) {
          firstPeeled = fieldIndex;
        }
        setPeeled(leaf->valueVector(), fieldIndex, context, maybePeeled);
      } else if (encoding == VectorEncoding::Simple::SEQUENCE) {
        if (firstIndices) {
          // having a mix of dictionary and sequence encoded fields
          peeled = false;
          break;
        }
        BufferPtr lengths = leaf->wrapInfo();
        if (!firstLengths) {
          firstLengths = std::move(lengths);
        } else if (lengths != firstLengths) {
          // different fields use different sequences
          peeled = false;
          break;
        }
        if (firstPeeled == -1) {
          firstPeeled = fieldIndex;
        }
        setPeeled(leaf->valueVector(), fieldIndex, context, maybePeeled);
      } else {
        // Non-peelable encoding.
        peeled = false;
        break;
      }
    }
    if (peeled) {
      ++numLevels;
      peeledVectors = std::move(maybePeeled);
    }
  } while (peeled && nonConstant);

  if (numLevels == 0 && nonConstant) {
    return std::make_pair(nullptr, nullptr);
  }

  // We peel off the wrappers and make a new selection.
  SelectivityVector* newRows;
  SelectivityVector* newFinalSelection;
  if (firstPeeled == -1) {
    // All the fields are constant across the rows of interest.
    newRows = singleRow(newRowsHolder, rows.begin());
    context->saveAndReset(saver, rows);
    context->wrapEncoding_ = VectorEncoding::Simple::CONSTANT;
    context->constantWrapIndex_ = rows.begin();
  } else {
    auto decoded = localDecoded.get();
    auto firstWrapper = context->getField(firstPeeled).get();
    const auto& rowsToDecode =
        context->isFinalSelection_ ? rows : *context->finalSelection_;
    decoded->makeIndices(*firstWrapper, rowsToDecode, numLevels);
    auto indices = decoded->indices();

    newRows = translateToInnerRows(rows, *decoded, newRowsHolder);

    if (!context->isFinalSelection_) {
      newFinalSelection = translateToInnerRows(
          *context->finalSelection_, *decoded, finalRowsHolder);
    }

    context->saveAndReset(saver, rows);

    if (!context->isFinalSelection_) {
      context->finalSelection_ = newFinalSelection;
    }

    setDictionaryWrapping(*decoded, rows, *firstWrapper, context);
  }
  int numPeeled = 0;
  for (int i = 0; i < peeledVectors.size(); ++i) {
    auto& values = peeledVectors[i];
    if (!values) {
      continue;
    }
    if (!constantFields.empty() && constantFields[i]) {
      context->setPeeled(
          i, BaseVector::wrapInConstant(rows.size(), rows.begin(), values));
    } else {
      context->setPeeled(i, values);
      ++numPeeled;
    }
  }
  // If the expression depends on one dictionary, results are cacheable.
  context->mayCache_ = numPeeled == 1 && constantFields.empty();
  return std::make_pair(std::move(newRows), std::move(newFinalSelection));
}

void Expr::evalEncodings(
    const SelectivityVector& rows,
    EvalCtx* context,
    VectorPtr* result) {
  if (deterministic_ && !distinctFields_.empty()) {
    bool hasNonFlat = false;
    for (const auto& field : distinctFields_) {
      if (!isFlat(*context->getField(field->index(context)))) {
        hasNonFlat = true;
        break;
      }
    }

    if (hasNonFlat) {
      LocalSelectivityVector newRowsHolder(context);
      LocalSelectivityVector finalRowsHolder(context);
      ContextSaver saveContext;
      LocalDecodedVector decodedHolder(context);
      auto [newRows, newFinalSelection] = peelEncodings(
          context,
          &saveContext,
          rows,
          decodedHolder,
          newRowsHolder,
          finalRowsHolder);
      if (newRows) {
        VectorPtr peeledResult;
        if (context->mayCache_) {
          context->mayCache_ = false;
          evalWithMemo(*newRows, context, &peeledResult);
        } else {
          evalWithNulls(*newRows, context, &peeledResult);
        }
        context->setWrapped(this, peeledResult, rows, result);
        return;
      }
    }
  }
  evalWithNulls(rows, context, result);
}

bool Expr::removeSureNulls(
    const SelectivityVector& rows,
    EvalCtx* context,
    LocalSelectivityVector& nullHolder) {
  SelectivityVector* result = nullptr;
  for (auto* field : distinctFields_) {
    VectorPtr values;
    field->evalSpecialForm(rows, context, &values);
    if (values->mayHaveNulls()) {
      auto nulls = values->flatRawNulls(rows);
      if (nulls) {
        if (!result) {
          result = nullHolder.get(rows.size());
          *result = rows;
        }
        auto bits = result->asMutableRange().bits();
        bits::andBits(bits, nulls, rows.begin(), rows.end());
      }
    }
  }
  if (result) {
    result->updateBounds();
    return true;
  }
  return false;
}

void Expr::addNulls(
    const SelectivityVector& rows,
    const uint64_t* rawNulls,
    EvalCtx* context,
    VectorPtr* result) {
  // If there's no `result` yet, return a NULL ContantVector.
  if (!*result) {
    *result =
        BaseVector::createNullConstant(type(), rows.size(), context->pool());
    return;
  }

  // If result is already a NULL ConstantVector, do nothing.
  if ((*result)->isConstantEncoding() && (*result)->mayHaveNulls()) {
    return;
  }

  if (!result->unique() || !(*result)->mayAddNulls()) {
    BaseVector::ensureWritable(
        SelectivityVector::empty(), type(), context->pool(), result);
  }
  if ((*result)->size() < rows.end()) {
    (*result)->resize(rows.end());
  }
  (*result)->addNulls(rawNulls, rows);
}

void Expr::evalWithNulls(
    const SelectivityVector& rows,
    EvalCtx* context,
    VectorPtr* result) {
  if (!rows.hasSelections()) {
    return;
  }

  if (propagatesNulls_) {
    bool mayHaveNulls = false;
    for (const auto& field : distinctFields_) {
      if (context->getField(field->index(context))->mayHaveNulls()) {
        mayHaveNulls = true;
        break;
      }
    }

    VarSetter scopedMayHaveNulls(context->mutableMayHaveNulls(), mayHaveNulls);

    if (mayHaveNulls && !distinctFields_.empty()) {
      LocalSelectivityVector nonNullHolder(context);
      if (removeSureNulls(rows, context, nonNullHolder)) {
        VarSetter noMoreNulls(context->mutableMayHaveNulls(), false);
        if (nonNullHolder.get()->hasSelections()) {
          evalAll(*nonNullHolder.get(), context, result);
        }
        auto rawNonNulls = nonNullHolder.get()->asRange().bits();
        addNulls(rows, rawNonNulls, context, result);
        return;
      }
    }
  }
  evalAll(rows, context, result);
}

namespace {
void deselectErrors(EvalCtx* context, SelectivityVector& rows) {
  auto errors = context->errors();
  if (!errors) {
    return;
  }
  // A non-null in errors resets the row. AND with the errors null mask.
  rows.deselectNonNulls(
      errors->rawNulls(), rows.begin(), std::min(errors->size(), rows.end()));
}
} // namespace

void Expr::evalWithMemo(
    const SelectivityVector& rows,
    EvalCtx* context,
    VectorPtr* result) {
  VectorPtr base;
  distinctFields_[0]->evalSpecialForm(rows, context, &base);
  ++numCachableInput_;
  if (baseDictionary_ == base) {
    ++numCacheableRepeats_;
    if (cachedDictionaryIndices_) {
      LocalSelectivityVector cachedHolder(context, rows);
      auto cached = cachedHolder.get();
      cached->intersect(*cachedDictionaryIndices_);
      if (cached->hasSelections()) {
        BaseVector::ensureWritable(rows, type(), context->pool(), result);
        (*result)->copy(dictionaryCache_.get(), *cached, nullptr);
      }
    }
    LocalSelectivityVector uncachedHolder(context, rows);
    auto uncached = uncachedHolder.get();
    if (cachedDictionaryIndices_) {
      uncached->deselect(*cachedDictionaryIndices_);
    }
    if (uncached->hasSelections()) {
      evalAll(*uncached, context, result);
      deselectErrors(context, *uncached);
      context->exprSet_->memoizingExprs_.insert(this);
      auto newCacheSize = uncached->end();
      if (cachedDictionaryIndices_->size() < newCacheSize) {
        int32_t oldSize = cachedDictionaryIndices_->size();
        cachedDictionaryIndices_->resize(newCacheSize);
        cachedDictionaryIndices_->setValidRange(oldSize, newCacheSize, false);
      }
      cachedDictionaryIndices_->select(*uncached);
      BaseVector::ensureWritable(
          *uncached, type(), context->pool(), &dictionaryCache_);
      dictionaryCache_->copy(result->get(), *uncached, nullptr);
    }
    return;
  }
  baseDictionary_ = base;
  evalAll(rows, context, result);
  dictionaryCache_ = *result;
  if (!cachedDictionaryIndices_) {
    cachedDictionaryIndices_ =
        context->execCtx()->getSelectivityVector(rows.end());
  }
  *cachedDictionaryIndices_ = rows;
  deselectErrors(context, *cachedDictionaryIndices_);
}

void Expr::setAllNulls(
    const SelectivityVector& rows,
    EvalCtx* context,
    VectorPtr* result) const {
  if (*result) {
    BaseVector::ensureWritable(rows, type(), context->pool(), result);
    LocalSelectivityVector notNulls(context, rows.end());
    notNulls.get()->setAll();
    notNulls.get()->deselect(rows);
    (*result)->addNulls(notNulls.get()->asRange().bits(), rows);
    return;
  }
  *result =
      BaseVector::createNullConstant(type(), rows.size(), context->pool());
}

namespace {
void scanVectorFunctionInputsStringEncoding(
    const VectorFunction* vectorFunction,
    std::vector<VectorPtr>& inputValues,
    EvalCtx* context,
    const SelectivityVector& rows) {
  std::vector<size_t> indices;
  if (vectorFunction->ensureStringEncodingSetAtAllInputs()) {
    for (auto i = 0; i < inputValues.size(); i++) {
      indices.push_back(i);
    }
  }
  for (auto& index : vectorFunction->ensureStringEncodingSetAt()) {
    indices.push_back(index);
  }

  // Compute string encoding for input vectors at indicies
  for (auto& index : indices) {
    // Some arguments are optionals and hence may not exist. And some functions
    // operates on dynamic types, but we only scan them when the type is string
    if (index < inputValues.size() &&
        inputValues[index]->type()->kind() == TypeKind::VARCHAR) {
      auto* vector =
          inputValues[index]->template as<SimpleVector<StringView>>();
      determineStringEncoding(context, vector, rows);
    }
  }
}

// Determine the string encoding of the result based on the inputs encoding if
// encoding propagation is configured. Return std::nullopt if not determined.
std::optional<functions::stringCore::StringEncodingMode>
getVectorFunctionResultStringEncoding(
    const VectorFunction* vectorFunction,
    std::vector<VectorPtr>& inputValues) {
  std::vector<size_t> indices;
  if (vectorFunction->propagateStringEncodingFromAllInputs()) {
    for (auto i = 0; i < inputValues.size(); i++) {
      indices.push_back(i);
    }
  } else if (vectorFunction->propagateStringEncodingFrom().has_value()) {
    indices = vectorFunction->propagateStringEncodingFrom().value();
  } else {
    // no propagation is done
    return std::nullopt;
  }

  auto resultEncoding = StringEncodingMode::ASCII;
  for (auto& index : indices) {
    // Some arguments are optionals and hence may not exist. And some functions
    // operates on dynamic types.
    if (index >= inputValues.size() ||
        inputValues[index]->type()->kind() != TypeKind::VARCHAR) {
      continue;
    }

    auto inputVector = inputValues[index]->as<SimpleVector<StringView>>();
    if (!inputVector->getStringEncoding().has_value()) {
      return std::nullopt;
    } else {
      resultEncoding =
          maxEncoding(resultEncoding, inputVector->getStringEncoding().value());
    }
  }
  return resultEncoding;
}

inline bool isPeelable(VectorEncoding::Simple encoding) {
  switch (encoding) {
    case VectorEncoding::Simple::CONSTANT:
    case VectorEncoding::Simple::DICTIONARY:
    case VectorEncoding::Simple::SEQUENCE:
      return true;
    default:
      return false;
  }
}
} // namespace

void Expr::evalAll(
    const SelectivityVector& rows,
    EvalCtx* context,
    VectorPtr* result) {
  if (!rows.hasSelections()) {
    return;
  }
  if (isSpecialForm()) {
    evalSpecialForm(rows, context, result);
    return;
  }
  LocalSelectivityVector nonNulls(context);
  auto* remainingRows = &rows;
  bool tryPeelArgs = deterministic_ ? true : false;
  bool defaultNulls = vectorFunction_->isDefaultNullBehavior();
  inputValues_.resize(inputs_.size());
  for (int32_t i = 0; i < inputs_.size(); ++i) {
    inputs_[i]->eval(*remainingRows, context, &inputValues_[i]);
    tryPeelArgs = tryPeelArgs && isPeelable(inputValues_[i]->encoding());
    if (defaultNulls && inputValues_[i]->mayHaveNulls()) {
      if (remainingRows == &rows) {
        nonNulls.allocate(rows.end());
        *nonNulls.get() = rows;
        remainingRows = nonNulls.get();
      }
      nonNulls.get()->deselectNulls(
          inputValues_[i]->flatRawNulls(rows),
          remainingRows->begin(),
          remainingRows->end());
      if (!remainingRows->hasSelections()) {
        inputValues_.clear();
        setAllNulls(rows, context, result);
        return;
      }
    }
  }

  if (!tryPeelArgs ||
      !applyFunctionWithPeeling(rows, *remainingRows, context, result)) {
    applyFunction(rows, *remainingRows, context, result);
  }
  if (remainingRows != &rows) {
    addNulls(rows, remainingRows->asRange().bits(), context, result);
  }
  inputValues_.clear();
}

namespace {
void setPeeledArg(
    VectorPtr arg,
    int32_t index,
    int32_t numArgs,
    std::vector<VectorPtr>& peeledArgs) {
  if (peeledArgs.empty()) {
    peeledArgs.resize(numArgs);
  }
  peeledArgs[index] = arg;
}
} // namespace

bool Expr::applyFunctionWithPeeling(
    const SelectivityVector& rows,
    const SelectivityVector& applyRows,
    EvalCtx* context,
    VectorPtr* result) {
  if (context->wrapEncoding_ == VectorEncoding::Simple::CONSTANT) {
    return false;
  }
  int numLevels = 0;
  bool peeled;
  bool nonConstant = false;
  auto numArgs = inputValues_.size();
  // Holds the outermost wrapper. This may be the last reference after
  // peeling for a temporary dictionary, hence use a shared_ptr.
  VectorPtr firstWrapper = nullptr;
  std::vector<bool> constantArgs;
  do {
    peeled = true;
    BufferPtr firstIndices;
    BufferPtr firstLengths;
    std::vector<VectorPtr> maybePeeled;
    for (auto i = 0; i < inputValues_.size(); ++i) {
      auto leaf = inputValues_[i];
      if (!constantArgs.empty() && constantArgs[i]) {
        setPeeledArg(leaf, i, numArgs, maybePeeled);
        continue;
      }
      if (numLevels == 0 && leaf->isConstant(rows)) {
        setPeeledArg(leaf, i, numArgs, maybePeeled);
        constantArgs.resize(numArgs);
        constantArgs.at(i) = true;
        continue;
      }
      nonConstant = true;
      auto encoding = leaf->encoding();
      if (encoding == VectorEncoding::Simple::DICTIONARY) {
        if (firstLengths) {
          // having a mix of dictionary and sequence encoded fields
          peeled = false;
          break;
        }
        if (!vectorFunction_->isDefaultNullBehavior() && leaf->rawNulls()) {
          // A dictionary that adds nulls over an Expr that is not null for a
          // null argument cannot be peeled.
          peeled = false;
          break;
        }
        BufferPtr indices = leaf->wrapInfo();
        if (!firstIndices) {
          firstIndices = std::move(indices);
        } else if (indices != firstIndices) {
          // different fields use different dictionaries
          peeled = false;
          break;
        }
        if (!firstWrapper) {
          firstWrapper = leaf;
        }
        setPeeledArg(leaf->valueVector(), i, numArgs, maybePeeled);
      } else if (encoding == VectorEncoding::Simple::SEQUENCE) {
        if (firstIndices) {
          // having a mix of dictionary and sequence encoded fields
          peeled = false;
          break;
        }
        BufferPtr lengths = leaf->wrapInfo();
        if (!firstLengths) {
          firstLengths = std::move(lengths);
        } else if (lengths != firstLengths) {
          // different fields use different sequences
          peeled = false;
          break;
        }
        if (!firstWrapper) {
          firstWrapper = leaf;
        }
        setPeeledArg(leaf->valueVector(), i, numArgs, maybePeeled);
      } else {
        // Non-peelable encoding.
        peeled = false;
        break;
      }
    }
    if (peeled) {
      ++numLevels;
      inputValues_ = std::move(maybePeeled);
    }
  } while (peeled && nonConstant);
  if (!numLevels) {
    return false;
  }
  LocalSelectivityVector newRowsHolder(context);
  ContextSaver saver;
  // We peel off the wrappers and make a new selection.
  SelectivityVector* newRows;
  LocalDecodedVector localDecoded(context);
  if (!firstWrapper) {
    // All the fields are constant across the rows of interest.
    newRows = singleRow(newRowsHolder, rows.begin());

    context->saveAndReset(&saver, rows);
    context->wrapEncoding_ = VectorEncoding::Simple::CONSTANT;
    context->constantWrapIndex_ = rows.begin();
  } else {
    auto decoded = localDecoded.get();
    decoded->makeIndices(*firstWrapper, applyRows, numLevels);
    newRows = translateToInnerRows(rows, *decoded, newRowsHolder);
    context->saveAndReset(&saver, rows);
    setDictionaryWrapping(*decoded, rows, *firstWrapper, context);
  }

  VectorPtr peeledResult;
  applyFunction(*newRows, *newRows, context, &peeledResult);
  context->setWrapped(this, peeledResult, rows, result);
  return true;
}

void Expr::applyFunction(
    const SelectivityVector& rows,
    const SelectivityVector& applyRows,
    EvalCtx* context,
    VectorPtr* result) {
  scanVectorFunctionInputsStringEncoding(
      vectorFunction_.get(), inputValues_, context, rows);
  auto resultEncoding = getVectorFunctionResultStringEncoding(
      vectorFunction_.get(), inputValues_);
  applyVectorFunction(applyRows, context, result);
  if (resultEncoding.has_value()) {
    (*result)->as<SimpleVector<StringView>>()->setStringEncoding(
        *resultEncoding);
  }
}

void Expr::applyVectorFunction(
    const SelectivityVector& rows,
    EvalCtx* context,
    VectorPtr* result) {
  // Single-argument deterministic functions expect their input as a flat
  // vector. Check if input has constant wrapping and remove it.
  if (deterministic_ && inputValues_.size() == 1 &&
      inputValues_[0]->isConstantEncoding()) {
    applySingleConstArgVectorFunction(rows, context, result);
  } else {
    vectorFunction_->apply(rows, inputValues_, this, context, result);
  }
}

void Expr::applySingleConstArgVectorFunction(
    const SelectivityVector& rows,
    EvalCtx* context,
    VectorPtr* result) {
  VELOX_CHECK_EQ(rows.countSelected(), 1);

  auto inputValue = inputValues_[0];

  auto resultRow = rows.begin();

  auto inputRow = inputValue->wrappedIndex(resultRow);
  LocalSelectivityVector rowHolder(context);
  auto inputRows = singleRow(rowHolder, inputRow);

  // VectorFunction expects flat input. If constant is of complex type, we can
  // use valueVector(). Otherwise, need to make a new flat vector.
  std::vector<VectorPtr> args;
  if (inputValue->isScalar()) {
    auto flat = BaseVector::create(inputValue->type(), 1, context->pool());
    flat->copy(inputValue.get(), 0, 0, 1);
    args = {flat};
  } else {
    args = {inputValue->valueVector()};
  }

  VectorPtr tempResult;
  vectorFunction_->apply(*inputRows, args, this, context, &tempResult);

  if (*result && !context->isFinalSelection()) {
    BaseVector::ensureWritable(rows, type(), context->pool(), result);
    (*result)->copy(tempResult.get(), resultRow, inputRow, 1);
  } else {
    // TODO Move is available only for flat vectors. Check if tempResult is
    // not flat and copy if so.
    if (inputRow < resultRow) {
      tempResult->resize(resultRow + 1);
    }
    tempResult->move(inputRow, resultRow);
    *result = std::move(tempResult);
  }
}

std::string Expr::toString() const {
  std::stringstream out;
  out << name_;
  if (!inputs_.empty()) {
    out << "(";
    for (auto i = 0; i < inputs_.size(); ++i) {
      if (i > 0) {
        out << ", ";
      }
      out << inputs_[i]->toString();
    }
    out << ")";
  }
  return out.str();
}

ExprSet::ExprSet(
    std::vector<std::shared_ptr<const core::ITypedExpr>>&& sources,
    core::ExecCtx* execCtx)
    : execCtx_(execCtx) {
  exprs_ = compileExpressions(std::move(sources), execCtx, this);
}

void ExprSet::eval(
    const SelectivityVector& rows,
    EvalCtx* ctx,
    std::vector<VectorPtr>* result) {
  eval(0, exprs_.size(), true, rows, ctx, result);
}

void ExprSet::eval(
    int32_t begin,
    int32_t end,
    bool initialize,
    const SelectivityVector& rows,
    EvalCtx* context,
    std::vector<VectorPtr>* result) {
  result->resize(exprs_.size());
  if (initialize) {
    clearSharedSubexprs();
  }
  for (int32_t i = begin; i < end; ++i) {
    exprs_[i]->eval(rows, context, &(*result)[i]);
  }
}

void ExprSet::clearSharedSubexprs() {
  for (auto& expr : toReset_) {
    expr->reset();
  }
}

void ExprSet::clear() {
  clearSharedSubexprs();
  for (auto* memo : memoizingExprs_) {
    memo->clearMemo();
  }
}

void determineStringEncoding(
    exec::EvalCtx* context,
    SimpleVector<StringView>* vector,
    const SelectivityVector& rows) {
  if (vector->getStringEncoding().has_value()) {
    return;
  }

  if (!vector->isConstantEncoding() &&
      !(rows.size() >= vector->size() && rows.isAllSelected())) {
    // TODO Allow setting string encoding for a subset of rows.
    return;
  }

  vector_size_t totalValuesCount = 0;
  vector_size_t totalAsciiCount = 0;

  auto countAscii = [&](const StringView& string) {
    totalValuesCount++;
    if (functions::stringCore::isAscii(string.data(), string.size())) {
      totalAsciiCount++;
      return true;
    }
    return false;
  };

  if (vector->isConstantEncoding()) {
    auto* constantVector = vector->as<ConstantVector<StringView>>();
    if (!constantVector->isNullAt(0)) {
      countAscii(*constantVector->rawValues());
    }
  } else if (vector->encoding() == VectorEncoding::Simple::FLAT) {
    auto* flatVector = vector->as<FlatVector<StringView>>();
    for (auto i = 0; i < flatVector->size(); i++) {
      if (!vector->isNullAt(i)) {
        countAscii(flatVector->valueAtFast(i));
      }
    }
  } else {
    // For the general case we use a decoder
    exec::LocalDecodedVector decodedVectorHolder(context);
    auto* decodedVector = decodedVectorHolder.get();

    SelectivityVector localRows(vector->size());
    decodedVector->decode(*vector, localRows);

    // if encoding of underlying vector is set then use it
    if (auto stringEncodung = decodedVector->base()
                                  ->as<SimpleVector<StringView>>()
                                  ->getStringEncoding()) {
      vector->setStringEncoding(*stringEncodung);
      return;
    }

    std::unordered_map<vector_size_t, bool> baseIndicesAscii;
    auto* indiciesMap = decodedVector->indices();

    for (auto i = 0; i < vector->size(); i++) {
      auto baseIndex = indiciesMap[i];

      // Make sure an index in the base vector is not proccessed multiple times
      if (!decodedVector->isNullAt(i)) {
        if (!baseIndicesAscii.count(baseIndex)) {
          baseIndicesAscii[baseIndex] =
              countAscii(decodedVector->valueAt<StringView>(i));
          continue;
        }
        // ascii already computed for underlying value
        totalValuesCount++;
        if (baseIndicesAscii[baseIndex]) {
          totalAsciiCount++;
        }
      }
    }
  }

  if (totalAsciiCount == totalValuesCount) {
    vector->setStringEncoding(functions::stringCore::StringEncodingMode::ASCII);
    return;
  }

  double asciiRatio = (double)totalAsciiCount / totalValuesCount;

  vector->setStringEncoding(
      asciiRatio > 0.80
          ? functions::stringCore::StringEncodingMode::MOSTLY_ASCII
          : functions::stringCore::StringEncodingMode::UTF8);
}

} // namespace facebook::velox::exec
