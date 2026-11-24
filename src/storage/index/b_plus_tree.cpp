#include <algorithm>
#include <cassert>
#include <fstream>
#include <string>
#include <utility>

#include "common/exception.h"
#include "common/logger.h"
#include "common/rid.h"
#include "storage/index/b_plus_tree.h"
#include "storage/page/header_page.h"

namespace bustub {
INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, BufferPoolManager *buffer_pool_manager, const KeyComparator &comparator,
                          int leaf_max_size, int internal_max_size)
    : index_name_(std::move(name)),
      root_page_id_(INVALID_PAGE_ID),
      buffer_pool_manager_(buffer_pool_manager),
      comparator_(comparator),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const -> bool { return root_page_id_ == INVALID_PAGE_ID; }

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType &key, std::vector<ValueType> *result, Transaction *transaction) -> bool {
  Transaction fallback_txn(0);
  Transaction *txn = transaction != nullptr ? transaction : &fallback_txn;
  LeafPage *leaf = FindLeafPage(key, false, txn, Operation::READ);
  if (leaf == nullptr) {
    return false;
  }
  int index = leaf->KeyIndex(key, comparator_);
  bool found = index < leaf->GetSize() && comparator_(leaf->KeyAt(index), key) == 0;
  if (found) {
    result->push_back(leaf->GetItem(index).second);
  }
  ReleaseLatchFromQueue(txn, Operation::READ);
  return found;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value, Transaction *transaction) -> bool {
  Transaction fallback_txn(0);
  Transaction *txn = transaction != nullptr ? transaction : &fallback_txn;
  if (root_page_id_ == INVALID_PAGE_ID) {
    root_latch_.WLock();
    if (root_page_id_ == INVALID_PAGE_ID) {
      page_id_t page_id;
      Page *page = buffer_pool_manager_->NewPage(&page_id);
      auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
      leaf->Init(page_id, INVALID_PAGE_ID, leaf_max_size_);
      leaf->GetItem(0) = {key, value};
      leaf->IncreaseSize(1);
      root_page_id_ = page_id;
      UpdateRootPageId(1);
      buffer_pool_manager_->UnpinPage(page_id, true);
      root_latch_.WUnlock();
      return true;
    }
    root_latch_.WUnlock();
  }

  LeafPage *leaf = FindLeafPage(key, false, txn, Operation::INSERT);
  if (leaf == nullptr) {
    return false;
  }

  int index = leaf->KeyIndex(key, comparator_);
  if (index < leaf->GetSize() && comparator_(leaf->KeyAt(index), key) == 0) {
    ReleaseLatchFromQueue(txn, Operation::INSERT);
    return false;
  }

  if (leaf->GetSize() < leaf->GetMaxSize()) {
    InsertIntoLeaf(leaf, key, value);
  } else {
    LeafPage *new_leaf = Split(leaf);
    InsertIntoParent(leaf, new_leaf->KeyAt(0), new_leaf, txn);
    LeafPage *target = comparator_(key, new_leaf->KeyAt(0)) < 0 ? leaf : new_leaf;
    InsertIntoLeaf(target, key, value);
    buffer_pool_manager_->UnpinPage(new_leaf->GetPageId(), true);
  }

  ReleaseLatchFromQueue(txn, Operation::INSERT);
  return true;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType &key, Transaction *transaction) {
  Transaction fallback_txn(0);
  Transaction *txn = transaction != nullptr ? transaction : &fallback_txn;
  if (root_page_id_ == INVALID_PAGE_ID) {
    return;
  }
  LeafPage *leaf = FindLeafPage(key, false, txn, Operation::DELETE);
  if (leaf == nullptr) {
    return;
  }
  int index = leaf->KeyIndex(key, comparator_);
  if (index >= leaf->GetSize() || comparator_(leaf->KeyAt(index), key) != 0) {
    ReleaseLatchFromQueue(txn, Operation::DELETE);
    return;
  }
  RemoveFromLeaf(leaf, index);
  if (leaf->GetSize() < leaf->GetMinSize()) {
    CoalesceOrRedistribute(leaf, txn);
  }
  ReleaseLatchFromQueue(txn, Operation::DELETE);
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin() -> INDEXITERATOR_TYPE {
  if (root_page_id_ == INVALID_PAGE_ID) {
    return INDEXITERATOR_TYPE(buffer_pool_manager_, nullptr, 0);
  }
  Page *page = buffer_pool_manager_->FetchPage(root_page_id_);
  auto *tree_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
  while (!tree_page->IsLeafPage()) {
    auto *internal = reinterpret_cast<InternalPage *>(tree_page);
    page_id_t child_id = internal->ValueAt(0);
    buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
    page = buffer_pool_manager_->FetchPage(child_id);
    tree_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
  }
  return INDEXITERATOR_TYPE(buffer_pool_manager_, page, 0);
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType &key) -> INDEXITERATOR_TYPE {
  if (root_page_id_ == INVALID_PAGE_ID) {
    return INDEXITERATOR_TYPE(buffer_pool_manager_, nullptr, 0);
  }
  Page *page = buffer_pool_manager_->FetchPage(root_page_id_);
  auto *tree_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
  while (!tree_page->IsLeafPage()) {
    auto *internal = reinterpret_cast<InternalPage *>(tree_page);
    page_id_t child_id = internal->Lookup(key, comparator_);
    buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
    page = buffer_pool_manager_->FetchPage(child_id);
    tree_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
  }
  auto *leaf = reinterpret_cast<LeafPage *>(tree_page);
  return INDEXITERATOR_TYPE(buffer_pool_manager_, page, leaf->KeyIndex(key, comparator_));
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End() -> INDEXITERATOR_TYPE { return INDEXITERATOR_TYPE(buffer_pool_manager_, nullptr, 0); }

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId() -> page_id_t { return root_page_id_; }

/*****************************************************************************
 * HELPER METHODS
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::FindLeafPage(const KeyType &key, bool left_most, Transaction *transaction, Operation op)
    -> LeafPage * {
  while (true) {
    page_id_t root_id = root_page_id_;
    if (root_id == INVALID_PAGE_ID) {
      return nullptr;
    }
    Page *page = buffer_pool_manager_->FetchPage(root_id);
    if (op == Operation::READ) {
      page->RLatch();
    } else {
      page->WLatch();
    }
    if (root_page_id_ != root_id) {
      if (op == Operation::READ) {
        page->RUnlatch();
      } else {
        page->WUnlatch();
      }
      buffer_pool_manager_->UnpinPage(root_id, false);
      continue;
    }
    transaction->AddIntoPageSet(page);

    auto *tree_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
    while (!tree_page->IsLeafPage()) {
      auto *internal = reinterpret_cast<InternalPage *>(tree_page);
      page_id_t child_id = left_most ? internal->ValueAt(0) : internal->Lookup(key, comparator_);
      Page *child = buffer_pool_manager_->FetchPage(child_id);
      if (op == Operation::READ) {
        child->RLatch();
      } else {
        child->WLatch();
      }
      transaction->AddIntoPageSet(child);
      if (IsSafe(reinterpret_cast<BPlusTreePage *>(child->GetData()), op)) {
        ReleaseAncestors(transaction, op);
      }
      page = child;
      tree_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
    }
    return reinterpret_cast<LeafPage *>(page->GetData());
  }
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetLatchedPage(Transaction *transaction, page_id_t page_id) -> Page * {
  for (auto *page : *transaction->GetPageSet()) {
    if (page->GetPageId() == page_id) {
      return page;
    }
  }
  return nullptr;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsSafe(const BPlusTreePage *node, Operation op) const -> bool {
  if (op == Operation::READ) {
    return true;
  }
  if (op == Operation::INSERT) {
    return node->GetSize() < node->GetMaxSize();
  }
  return node->GetSize() > node->GetMinSize();
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ReleaseAncestors(Transaction *transaction, Operation op) {
  auto page_set = transaction->GetPageSet();
  while (page_set->size() > 1) {
    Page *page = page_set->front();
    page_set->pop_front();
    if (op == Operation::READ) {
      page->RUnlatch();
    } else {
      page->WUnlatch();
    }
    buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
  }
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ReleaseLatchFromQueue(Transaction *transaction, Operation op) {
  auto page_set = transaction->GetPageSet();
  while (!page_set->empty()) {
    Page *page = page_set->front();
    page_set->pop_front();
    if (op == Operation::READ) {
      page->RUnlatch();
    } else {
      page->WUnlatch();
    }
    buffer_pool_manager_->UnpinPage(page->GetPageId(), op != Operation::READ);
  }
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertIntoLeaf(LeafPage *leaf, const KeyType &key, const ValueType &value) {
  int index = leaf->KeyIndex(key, comparator_);
  int size = leaf->GetSize();
  for (int i = size; i > index; i--) {
    leaf->GetItem(i) = leaf->GetItem(i - 1);
  }
  leaf->GetItem(index) = {key, value};
  leaf->IncreaseSize(1);
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Split(LeafPage *leaf) -> LeafPage * {
  page_id_t page_id;
  Page *page = buffer_pool_manager_->NewPage(&page_id);
  auto *new_leaf = reinterpret_cast<LeafPage *>(page->GetData());
  new_leaf->Init(page_id, leaf->GetParentPageId(), leaf_max_size_);
  int size = leaf->GetSize();
  int split = size / 2;
  for (int i = split; i < size; i++) {
    new_leaf->GetItem(i - split) = leaf->GetItem(i);
  }
  leaf->SetSize(split);
  new_leaf->SetSize(size - split);
  new_leaf->SetNextPageId(leaf->GetNextPageId());
  leaf->SetNextPageId(page_id);
  return new_leaf;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Split(InternalPage *node) -> std::pair<InternalPage *, KeyType> {
  page_id_t page_id;
  Page *page = buffer_pool_manager_->NewPage(&page_id);
  auto *new_node = reinterpret_cast<InternalPage *>(page->GetData());
  new_node->Init(page_id, node->GetParentPageId(), internal_max_size_ - 1);

  int size = node->GetSize();
  int start = node->GetMinSize() + 1;
  KeyType separator = node->KeyAt(start);
  for (int i = start; i <= size; i++) {
    new_node->GetItem(i - start) = node->GetItem(i);
  }
  node->SetSize(start - 1);
  new_node->SetSize(size - start);

  for (int i = 0; i <= new_node->GetSize(); i++) {
    UpdateChildParent(new_node->ValueAt(i), page_id);
  }
  return {new_node, separator};
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertIntoInternal(InternalPage *parent, int index, const KeyType &key,
                                        const BPlusTreePage *new_node) {
  int size = parent->GetSize();
  for (int i = size; i > index; i--) {
    parent->GetItem(i + 1) = parent->GetItem(i);
  }
  parent->GetItem(index + 1) = {key, new_node->GetPageId()};
  parent->IncreaseSize(1);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertIntoParent(BPlusTreePage *old_node, const KeyType &key, BPlusTreePage *new_node,
                                      Transaction *transaction) {
  if (old_node->IsRootPage()) {
    page_id_t page_id;
    Page *page = buffer_pool_manager_->NewPage(&page_id);
    auto *new_root = reinterpret_cast<InternalPage *>(page->GetData());
    new_root->Init(page_id, INVALID_PAGE_ID, internal_max_size_ - 1);
    new_root->GetItem(0) = {key, old_node->GetPageId()};
    new_root->GetItem(1) = {key, new_node->GetPageId()};
    new_root->SetSize(1);
    old_node->SetParentPageId(page_id);
    new_node->SetParentPageId(page_id);
    root_page_id_ = page_id;
    UpdateRootPageId(0);
    buffer_pool_manager_->UnpinPage(page_id, true);
    return;
  }

  Page *parent_page = GetLatchedPage(transaction, old_node->GetParentPageId());
  if (parent_page == nullptr) {
    parent_page = buffer_pool_manager_->FetchPage(old_node->GetParentPageId());
    parent_page->WLatch();
    transaction->AddIntoPageSet(parent_page);
  }
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());

  if (parent->GetSize() < parent->GetMaxSize()) {
    int index = parent->ValueIndex(old_node->GetPageId());
    InsertIntoInternal(parent, index, key, new_node);
    new_node->SetParentPageId(parent->GetPageId());
    return;
  }

  auto [new_internal, separator] = Split(parent);
  InsertIntoParent(parent, separator, new_internal, transaction);
  InternalPage *target = parent->ValueIndex(old_node->GetPageId()) != -1 ? parent : new_internal;
  int index = target->ValueIndex(old_node->GetPageId());
  InsertIntoInternal(target, index, key, new_node);
  new_node->SetParentPageId(target->GetPageId());
  buffer_pool_manager_->UnpinPage(new_internal->GetPageId(), true);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromLeaf(LeafPage *leaf, int index) {
  int size = leaf->GetSize();
  for (int i = index; i < size - 1; i++) {
    leaf->GetItem(i) = leaf->GetItem(i + 1);
  }
  leaf->IncreaseSize(-1);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromParent(InternalPage *parent, int index) {
  int size = parent->GetSize();
  for (int i = index; i < size; i++) {
    parent->GetItem(i) = parent->GetItem(i + 1);
  }
  parent->IncreaseSize(-1);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::UpdateChildParent(page_id_t child_id, page_id_t parent_id) {
  Page *child_page = buffer_pool_manager_->FetchPage(child_id);
  auto *child = reinterpret_cast<BPlusTreePage *>(child_page->GetData());
  child->SetParentPageId(parent_id);
  buffer_pool_manager_->UnpinPage(child_id, true);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ReleasePageAndDelete(Transaction *transaction, Page *page) {
  auto page_set = transaction->GetPageSet();
  for (auto it = page_set->begin(); it != page_set->end(); ++it) {
    if (*it == page) {
      page_set->erase(it);
      break;
    }
  }
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
  transaction->AddIntoDeletedPageSet(page->GetPageId());
  buffer_pool_manager_->DeletePage(page->GetPageId());
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::AdjustRoot(BPlusTreePage *node, Transaction *transaction) {
  if (node->IsLeafPage()) {
    if (node->GetSize() == 0) {
      root_page_id_ = INVALID_PAGE_ID;
      UpdateRootPageId(0);
      Page *page = GetLatchedPage(transaction, node->GetPageId());
      ReleasePageAndDelete(transaction, page);
    }
    return;
  }

  auto *internal = reinterpret_cast<InternalPage *>(node);
  if (internal->GetSize() == 0) {
    page_id_t child_id = internal->ValueAt(0);
    root_page_id_ = child_id;
    UpdateRootPageId(0);
    UpdateChildParent(child_id, INVALID_PAGE_ID);
    Page *page = GetLatchedPage(transaction, node->GetPageId());
    ReleasePageAndDelete(transaction, page);
  }
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::CoalesceOrRedistribute(BPlusTreePage *node, Transaction *transaction) {
  if (node->IsRootPage()) {
    AdjustRoot(node, transaction);
    return;
  }

  Page *parent_page = GetLatchedPage(transaction, node->GetParentPageId());
  if (parent_page == nullptr) {
    parent_page = buffer_pool_manager_->FetchPage(node->GetParentPageId());
    parent_page->WLatch();
    transaction->AddIntoPageSet(parent_page);
  }
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
  int node_index = parent->ValueIndex(node->GetPageId());
  int sibling_index = node_index > 0 ? node_index - 1 : node_index + 1;

  Page *sibling_page = buffer_pool_manager_->FetchPage(parent->ValueAt(sibling_index));
  sibling_page->WLatch();
  auto *sibling = reinterpret_cast<BPlusTreePage *>(sibling_page->GetData());

  if (node->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(node);
    auto *sibling_leaf = reinterpret_cast<LeafPage *>(sibling);
    if (sibling->GetSize() > sibling->GetMinSize()) {
      if (sibling_index < node_index) {
        int sibling_size = sibling_leaf->GetSize();
        for (int i = leaf->GetSize(); i > 0; i--) {
          leaf->GetItem(i) = leaf->GetItem(i - 1);
        }
        leaf->GetItem(0) = sibling_leaf->GetItem(sibling_size - 1);
        leaf->IncreaseSize(1);
        sibling_leaf->IncreaseSize(-1);
        parent->SetKeyAt(node_index, leaf->KeyAt(0));
      } else {
        leaf->GetItem(leaf->GetSize()) = sibling_leaf->GetItem(0);
        leaf->IncreaseSize(1);
        int sibling_size = sibling_leaf->GetSize();
        for (int i = 0; i < sibling_size - 1; i++) {
          sibling_leaf->GetItem(i) = sibling_leaf->GetItem(i + 1);
        }
        sibling_leaf->IncreaseSize(-1);
        parent->SetKeyAt(node_index + 1, sibling_leaf->KeyAt(0));
      }
      sibling_page->WUnlatch();
      buffer_pool_manager_->UnpinPage(sibling_page->GetPageId(), true);
      return;
    }

    if (sibling_index < node_index) {
      int sibling_size = sibling_leaf->GetSize();
      int leaf_size = leaf->GetSize();
      for (int i = 0; i < leaf_size; i++) {
        sibling_leaf->GetItem(sibling_size + i) = leaf->GetItem(i);
      }
      sibling_leaf->SetSize(sibling_size + leaf_size);
      sibling_leaf->SetNextPageId(leaf->GetNextPageId());
      RemoveFromParent(parent, node_index);
      sibling_page->WUnlatch();
      buffer_pool_manager_->UnpinPage(sibling_page->GetPageId(), true);
      Page *leaf_page = GetLatchedPage(transaction, leaf->GetPageId());
      ReleasePageAndDelete(transaction, leaf_page);
    } else {
      int leaf_size = leaf->GetSize();
      int sibling_size = sibling_leaf->GetSize();
      for (int i = 0; i < sibling_size; i++) {
        leaf->GetItem(leaf_size + i) = sibling_leaf->GetItem(i);
      }
      leaf->SetSize(leaf_size + sibling_size);
      leaf->SetNextPageId(sibling_leaf->GetNextPageId());
      RemoveFromParent(parent, sibling_index);
      sibling_page->WUnlatch();
      buffer_pool_manager_->UnpinPage(sibling_page->GetPageId(), false);
      transaction->AddIntoDeletedPageSet(sibling_page->GetPageId());
      buffer_pool_manager_->DeletePage(sibling_page->GetPageId());
    }
    if (parent->GetSize() < parent->GetMinSize()) {
      CoalesceOrRedistribute(parent, transaction);
    }
    return;
  }

  auto *internal = reinterpret_cast<InternalPage *>(node);
  auto *sibling_internal = reinterpret_cast<InternalPage *>(sibling);
  if (sibling->GetSize() > sibling->GetMinSize()) {
    if (sibling_index < node_index) {
      int sibling_size = sibling_internal->GetSize();
      KeyType separator = parent->KeyAt(node_index);
      KeyType sibling_last_key = sibling_internal->KeyAt(sibling_size);
      page_id_t sibling_last_child = sibling_internal->ValueAt(sibling_size);
      int node_size = internal->GetSize();
      for (int i = node_size; i >= 0; i--) {
        internal->GetItem(i + 1) = internal->GetItem(i);
      }
      internal->GetItem(0) = {separator, sibling_last_child};
      internal->SetKeyAt(1, separator);
      internal->SetSize(node_size + 1);
      sibling_internal->SetSize(sibling_size - 1);
      parent->SetKeyAt(node_index, sibling_last_key);
      UpdateChildParent(sibling_last_child, internal->GetPageId());
    } else {
      int node_size = internal->GetSize();
      KeyType separator = parent->KeyAt(node_index + 1);
      KeyType sibling_first_key = sibling_internal->KeyAt(1);
      page_id_t sibling_first_child = sibling_internal->ValueAt(0);
      internal->GetItem(node_size + 1) = {separator, sibling_first_child};
      internal->SetSize(node_size + 1);
      int sibling_size = sibling_internal->GetSize();
      page_id_t sibling_new_first_child = sibling_internal->ValueAt(1);
      for (int i = 1; i < sibling_size; i++) {
        sibling_internal->GetItem(i) = sibling_internal->GetItem(i + 1);
      }
      sibling_internal->GetItem(0) = {sibling_first_key, sibling_new_first_child};
      sibling_internal->SetSize(sibling_size - 1);
      parent->SetKeyAt(node_index + 1, sibling_first_key);
      UpdateChildParent(sibling_first_child, internal->GetPageId());
    }
    sibling_page->WUnlatch();
    buffer_pool_manager_->UnpinPage(sibling_page->GetPageId(), true);
    return;
  }

  if (sibling_index < node_index) {
    int sibling_size = sibling_internal->GetSize();
    int node_size = internal->GetSize();
    KeyType separator = parent->KeyAt(node_index);
    page_id_t node_first_child = internal->ValueAt(0);
    sibling_internal->GetItem(sibling_size + 1) = {separator, node_first_child};
    for (int i = 1; i <= node_size; i++) {
      sibling_internal->GetItem(sibling_size + 1 + i) = internal->GetItem(i);
    }
    sibling_internal->SetSize(sibling_size + 1 + node_size);
    for (int i = 0; i <= node_size; i++) {
      UpdateChildParent(internal->ValueAt(i), sibling_internal->GetPageId());
    }
    RemoveFromParent(parent, node_index);
    sibling_page->WUnlatch();
    buffer_pool_manager_->UnpinPage(sibling_page->GetPageId(), true);
    Page *node_page = GetLatchedPage(transaction, internal->GetPageId());
    ReleasePageAndDelete(transaction, node_page);
  } else {
    int node_size = internal->GetSize();
    int sibling_size = sibling_internal->GetSize();
    KeyType separator = parent->KeyAt(node_index + 1);
    page_id_t sibling_first_child = sibling_internal->ValueAt(0);
    internal->GetItem(node_size + 1) = {separator, sibling_first_child};
    for (int i = 1; i <= sibling_size; i++) {
      internal->GetItem(node_size + 1 + i) = sibling_internal->GetItem(i);
    }
    internal->SetSize(node_size + 1 + sibling_size);
    for (int i = 0; i <= sibling_size; i++) {
      UpdateChildParent(sibling_internal->ValueAt(i), internal->GetPageId());
    }
    RemoveFromParent(parent, sibling_index);
    sibling_page->WUnlatch();
    buffer_pool_manager_->UnpinPage(sibling_page->GetPageId(), false);
    transaction->AddIntoDeletedPageSet(sibling_page->GetPageId());
    buffer_pool_manager_->DeletePage(sibling_page->GetPageId());
  }
  if (parent->GetSize() < parent->GetMinSize()) {
    CoalesceOrRedistribute(parent, transaction);
  }
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Update/Insert root page id in header page(where page_id = 0, header_page is
 * defined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      default value is false. When set to true,
 * insert a record <index_name, root_page_id> into header page instead of
 * updating it.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::UpdateRootPageId(int insert_record) {
  auto *header_page = static_cast<HeaderPage *>(buffer_pool_manager_->FetchPage(HEADER_PAGE_ID));
  if (insert_record != 0) {
    // create a new record<index_name + root_page_id> in header_page
    header_page->InsertRecord(index_name_, root_page_id_);
  } else {
    // update root_page_id in header_page
    header_page->UpdateRecord(index_name_, root_page_id_);
  }
  buffer_pool_manager_->UnpinPage(HEADER_PAGE_ID, true);
}

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string &file_name, Transaction *transaction) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;

    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, transaction);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string &file_name, Transaction *transaction) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, transaction);
  }
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Draw(BufferPoolManager *bpm, const std::string &outf) {
  if (IsEmpty()) {
    LOG_WARN("Draw an empty tree");
    return;
  }
  std::ofstream out(outf);
  out << "digraph G {" << std::endl;
  ToGraph(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(root_page_id_)->GetData()), bpm, out);
  out << "}" << std::endl;
  out.flush();
  out.close();
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Print(BufferPoolManager *bpm) {
  if (IsEmpty()) {
    LOG_WARN("Print an empty tree");
    return;
  }
  ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(root_page_id_)->GetData()), bpm);
}

/**
 * This method is used for debug only, You don't need to modify
 * @tparam KeyType
 * @tparam ValueType
 * @tparam KeyComparator
 * @param page
 * @param bpm
 * @param out
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    // Print node name
    out << leaf_prefix << leaf->GetPageId();
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      out << "<TD>" << leaf->KeyAt(i) << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << leaf->GetPageId() << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << leaf->GetPageId() << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }

    // Print parent links if there is a parent
    if (leaf->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << leaf_prefix
          << leaf->GetPageId() << ";\n";
    }
  } else {
    auto *inner = reinterpret_cast<InternalPage *>(page);
    // Print node name
    out << internal_prefix << inner->GetPageId();
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        out << inner->KeyAt(i);
      } else {
        out << " ";
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Parent link
    if (inner->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << internal_prefix
          << inner->GetPageId() << ";\n";
    }
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, bpm, out);
      if (i > 0) {
        auto sibling_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_page->GetPageId() << " " << internal_prefix
              << child_page->GetPageId() << "};\n";
        }
        bpm->UnpinPage(sibling_page->GetPageId(), false);
      }
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

/**
 * This function is for debug only, you don't need to modify
 * @tparam KeyType
 * @tparam ValueType
 * @tparam KeyComparator
 * @param page
 * @param bpm
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
              << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(internal->ValueAt(i))->GetData()), bpm);
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;
template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;
template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;
template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;
template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
