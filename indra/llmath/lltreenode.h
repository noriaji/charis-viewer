/**
 * @file lltreenode.h
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLTREENODE_H
#define LL_LLTREENODE_H

#include "stdtypes.h"
#include "xform.h"
#include "llpointer.h"
#include "llrefcount.h"

#include <vector>

template <class T> class LLTreeNode;
template <class T> class LLTreeTraveler;
template <class T> class LLTreeListener;

template <class T>
class LLTreeListener: public LLRefCount
{
public:
    virtual void handleInsertion(const LLTreeNode<T>* node, T* data) = 0;
    virtual void handleRemoval(const LLTreeNode<T>* node, T* data) = 0;
    virtual void handleDestruction(const LLTreeNode<T>* node) = 0;
    virtual void handleStateChange(const LLTreeNode<T>* node) = 0;
};

template <class T>
class LLTreeNode
{
public:
    virtual ~LLTreeNode();

    virtual bool insert(T* data);
    virtual bool remove(T* data);
    virtual void notifyRemoval(T* data);
    virtual U32 hasListeners() const { return !mListeners.empty(); }
    virtual U32 getListenerCount() const { return static_cast<U32>(mListeners.size()); }
    // LLRHI: not virtual (nothing overrides it), and the first listener (an octree
    // node's group) is also kept in the node itself, so the culls' getListener(0)
    // on every node is one load instead of a call and two (research/48).
    LLTreeListener<T>* getListener(U32 index) const
    {
        if (index == 0)
        {
            return mFirstListener;
        }
        if (index < mListeners.size())
        {
            return mListeners[index];
        }
        return NULL;
    }
    virtual void addListener(LLTreeListener<T>* listener)
    {
        mListeners.push_back(listener);
        mFirstListener = mListeners[0];
    }

protected:
    void destroyListeners()
    {
        for (U32 i = 0; i < mListeners.size(); i++)
        {
            mListeners[i]->handleDestruction(this);
        }
        mListeners.clear();
        mFirstListener = NULL;
    }

public:
    // Add and remove listeners only with addListener and destroyListeners (mFirstListener follows them).
    std::vector<LLPointer<LLTreeListener<T> > > mListeners;
private:
    LLTreeListener<T>* mFirstListener = NULL;   // mListeners[0], held by it
};

template <class T>
class LLTreeTraveler
{
public:
    virtual ~LLTreeTraveler() { };
    virtual void traverse(const LLTreeNode<T>* node) = 0;
    virtual void visit(const LLTreeNode<T>* node) = 0;
};

template <class T>
LLTreeNode<T>::~LLTreeNode()
{
    destroyListeners();
};

template <class T>
bool LLTreeNode<T>::insert(T* data)
{
    for (U32 i = 0; i < mListeners.size(); i++)
    {
        mListeners[i]->handleInsertion(this, data);
    }
    return true;
};

template <class T>
bool LLTreeNode<T>::remove(T* data)
{
    return true;
};

template <class T>
void LLTreeNode<T>::notifyRemoval(T* data)
{
    for (U32 i = 0; i < mListeners.size(); i++)
    {
        mListeners[i]->handleRemoval(this, data);
    }
}

#endif
