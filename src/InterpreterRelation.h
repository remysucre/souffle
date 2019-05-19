/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file InterpreterRelation.h
 *
 * Defines Interpreter Relations
 *
 ***********************************************************************/

#pragma once

#include "InterpreterIndex.h"
#include "ParallelUtils.h"
#include "RamIndexAnalysis.h"
#include "RamTypes.h"

#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace souffle {

/**
 * Interpreter Relation
 */
class InterpreterRelation {
    using LexOrder = std::vector<int>;

public:
    InterpreterRelation(size_t relArity, const MinIndexSelection* orderSet)
            : arity(relArity), orderSet(orderSet) {
        // Create all necessary indices based on orderSet
        for (auto& order : orderSet->getAllOrders()) {
            indices.push_back(InterpreterIndex(order));
        }
        if (orderSet->getAllOrders().size() == 0) {
            indices.push_back(InterpreterIndex(LexOrder()));
        }
    }

    InterpreterRelation(const InterpreterRelation& other) = delete;

    virtual ~InterpreterRelation() = default;

    /** Set AttributeType for the relation */
    void setAttributes(const std::vector<std::string> attributeTypes) {
        attributeTypeQualifiers = attributeTypes;
    }

    /** Get AttributeType for the relation */
    std::vector<std::string>& getAttributeTypeQualifiers() {
        return attributeTypeQualifiers;
    }

    /** Get arity of relation */
    size_t getArity() const {
        return arity;
    }

    /** Check whether relation is empty */
    bool empty() const {
        return num_tuples == 0;
    }

    /** Gets the number of contained tuples */
    size_t size() const {
        return num_tuples;
    }

    /** Insert tuple */
    virtual void insert(const RamDomain* tuple) {
        assert(tuple);

        // make existence check
        if (exists(tuple)) {
            return;
        }

        // check for null-arity
        if (arity == 0) {
           indices[0].insert(tuple);
           num_tuples = 1;
           return;
        }

        int blockIndex = num_tuples / (BLOCK_SIZE / arity);
        int tupleIndex = (num_tuples % (BLOCK_SIZE / arity)) * arity;

        if (tupleIndex == 0) {
            blockList.push_back(std::make_unique<RamDomain[]>(BLOCK_SIZE));
        }

        RamDomain* newTuple = &blockList[blockIndex][tupleIndex];
        for (size_t i = 0; i < arity; ++i) {
            newTuple[i] = tuple[i];
        }

        // update all indexes with new tuple
        for (auto& cur : indices) {
            cur.insert(newTuple);
        }

        // increment relation size
        num_tuples++;
    }

    /** Merge another relation into this relation */
    void insert(const InterpreterRelation& other) {
        assert(getArity() == other.getArity());
        for (const auto& cur : other) {
            insert(cur);
        }
    }

    /** Purge table */
    void purge() {
        blockList.clear();
        for (auto& cur : indices) {
            cur.purge();
        }
        num_tuples = 0;
    }

    /** get index for a given search signature. Order are encoded as bits for each column */
    InterpreterIndex* getIndex(const SearchSignature& col) const {
        return getIndexByPos(orderSet->getLexOrderNum(col));
    }

    /** get index for a given order. Order are encoded as bits for each column */
    InterpreterIndex* getIndexByPos(int idx) const {
        return &indices[idx];
    }

    /** Obtains a full index-key for this relation */
    SearchSignature getTotalIndexKey() const {
        return (1 << (getArity())) - 1;
    }

    /** check whether a tuple exists in the relation */
    bool exists(const RamDomain* tuple) const {
        // handle arity 0
        if (getArity() == 0) {
            //return !empty();
            InterpreterIndex* index = getIndexByPos(0);
            return index->exists(tuple);
        }
        InterpreterIndex* index = getIndex(getTotalIndexKey());
        return index->exists(tuple);
    }

    void setLevel(size_t level) {
        this->level = level;
    }

    size_t getLevel() {
        return this->level;
    }

    // --- iterator ---

    /** Iterator for relation */
    class iterator : public std::iterator<std::forward_iterator_tag, RamDomain*> {
    public:
        iterator() = default;

        iterator(const InterpreterRelation* const relation)
                : relation(relation), tuple(relation->arity == 0 ? reinterpret_cast<RamDomain*>(this)
                                                                 : &relation->blockList[0][0]) {}

        const RamDomain* operator*() {
            return tuple;
        }

        bool operator==(const iterator& other) const {
            return tuple == other.tuple;
        }

        bool operator!=(const iterator& other) const {
            return (tuple != other.tuple);
        }

        iterator& operator++() {
            // support 0-arity
            if (relation->arity == 0) {
                tuple = nullptr;
                return *this;
            }

            // support all other arities
            ++index;
            if (index == relation->num_tuples) {
                tuple = nullptr;
                return *this;
            }

            int blockIndex = index / (BLOCK_SIZE / relation->arity);
            int tupleIndex = (index % (BLOCK_SIZE / relation->arity)) * relation->arity;

            tuple = &relation->blockList[blockIndex][tupleIndex];
            return *this;
        }

    private:
        const InterpreterRelation* relation = nullptr;
        size_t index = 0;
        RamDomain* tuple = nullptr;
    };

    /** get iterator begin of relation */
    inline iterator begin() const {
        // check for emptiness
        if (empty()) {
            return end();
        }

        return iterator(this);
    }

    /** get iterator begin of relation */
    inline iterator end() const {
        return iterator();
    }

    /** Extend tuple */
    virtual std::vector<RamDomain*> extend(const RamDomain* tuple) {
        std::vector<RamDomain*> newTuples;

        // A standard relation does not generate extra new knowledge on insertion.
        newTuples.push_back(new RamDomain[2]{tuple[0], tuple[1]});

        return newTuples;
    }

    /** Extend relation */
    virtual void extend(const InterpreterRelation& rel) {}

    /** Index for zero-arity relation */
   // using index_set = btree_multiset<const RamDomain*, InterpreterIndex::comparator,
   //         std::allocator<const RamDomain*>, 512>;
   // static index_set set;

private:
    /** Arity of relation */
    const size_t arity;

    /** Size of blocks containing tuples */
    static const int BLOCK_SIZE = 1024;

    /** Number of tuples in relation */
    size_t num_tuples = 0;

    std::deque<std::unique_ptr<RamDomain[]>> blockList;

    /** List of indices */
    mutable std::vector<InterpreterIndex> indices;

    /** IndexSet */
    const MinIndexSelection* orderSet;

    /** Lock for parallel execution */
    mutable Lock lock;

    /** Type of attributes */
    std::vector<std::string> attributeTypeQualifiers;

    /** Stratum level information */
    size_t level = 0;
};

/**
 * Interpreter Equivalence Relation
 */

class InterpreterEqRelation : public InterpreterRelation {
public:
    InterpreterEqRelation(size_t relArity, const MinIndexSelection* orderSet)
            : InterpreterRelation(relArity, orderSet) {}

    /** Insert tuple */
    void insert(const RamDomain* tuple) override {
        // TODO: (pnappa) an eqrel check here is all that appears to be needed for implicit additions
        // TODO: future optimisation would require this as a member datatype
        // brave soul required to pass this quest
        // // specialisation for eqrel defs
        // std::unique_ptr<binaryrelation> eqreltuples;
        // in addition, it requires insert functions to insert into that, and functions
        // which allow reading of stored values must be changed to accommodate.
        // e.g. insert =>  eqRelTuples->insert(tuple[0], tuple[1]);

        // for now, we just have a naive & extremely slow version, otherwise known as a O(n^2) insertion
        // ):

        for (auto* newTuple : extend(tuple)) {
            InterpreterRelation::insert(newTuple);
            delete[] newTuple;
        }
    }

    /** Find the new knowledge generated by inserting a tuple */
    std::vector<RamDomain*> extend(const RamDomain* tuple) override {
        std::vector<RamDomain*> newTuples;

        newTuples.push_back(new RamDomain[2]{tuple[0], tuple[0]});
        newTuples.push_back(new RamDomain[2]{tuple[0], tuple[1]});
        newTuples.push_back(new RamDomain[2]{tuple[1], tuple[0]});
        newTuples.push_back(new RamDomain[2]{tuple[1], tuple[1]});

        std::vector<const RamDomain*> relevantStored;
        for (const RamDomain* vals : *this) {
            if (vals[0] == tuple[0] || vals[0] == tuple[1] || vals[1] == tuple[0] || vals[1] == tuple[1]) {
                relevantStored.push_back(vals);
            }
        }

        for (const auto vals : relevantStored) {
            newTuples.push_back(new RamDomain[2]{vals[0], tuple[0]});
            newTuples.push_back(new RamDomain[2]{vals[0], tuple[1]});
            newTuples.push_back(new RamDomain[2]{vals[1], tuple[0]});
            newTuples.push_back(new RamDomain[2]{vals[1], tuple[1]});
            newTuples.push_back(new RamDomain[2]{tuple[0], vals[0]});
            newTuples.push_back(new RamDomain[2]{tuple[0], vals[1]});
            newTuples.push_back(new RamDomain[2]{tuple[1], vals[0]});
            newTuples.push_back(new RamDomain[2]{tuple[1], vals[1]});
        }

        return newTuples;
    }
    /** Extend this relation with new knowledge generated by inserting all tuples from a relation */
    void extend(const InterpreterRelation& rel) override {
        std::vector<RamDomain*> newTuples;
        // store all values that will be implicitly relevant to the those that we will insert
        for (const auto* tuple : rel) {
            for (auto* newTuple : extend(tuple)) {
                newTuples.push_back(newTuple);
            }
        }
        for (const auto* newTuple : newTuples) {
            InterpreterRelation::insert(newTuple);
            delete[] newTuple;
        }
    }
};

}  // end of namespace souffle
