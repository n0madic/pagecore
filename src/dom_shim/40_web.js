(function(root, factory) {
  const definition = factory();
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = definition;
  } else if (root && typeof root.__pagecore_dom_shim_define === 'function') {
    root.__pagecore_dom_shim_define(definition);
  } else if (root) {
    root.PageCoreDomShimModules = root.PageCoreDomShimModules || {};
    root.PageCoreDomShimModules[definition.name] = definition;
  }
})(globalThis, function() {
  'use strict';

  return {
    name: 'web',
    deps: ['core', 'events', 'dom'],
    install(ctx, api) {
      const { global, host, bridge } = ctx;
      const {
        defineValue,
        assertNode,
        absoluteURL,
        activityBegin,
        activityEnd,
        formatErrorForLog,
        loadHostResourceAsync: coreLoadHostResourceAsync,
        cancelResourceLoad
      } = api.core;
      const {
        DOMException,
        EventTarget,
        Event,
        MessageEvent,
        KeyboardEvent,
        MouseEvent,
        PointerEvent
      } = api.events;
      const { document, fragmentFromHTML, Element } = api.dom;


        class DOMRectReadOnly {
          constructor(x = 0, y = 0, width = 0, height = 0) {
            this.x = Number.isFinite(Number(x)) ? Number(x) : 0;
            this.y = Number.isFinite(Number(y)) ? Number(y) : 0;
            this.width = Number.isFinite(Number(width)) ? Number(width) : 0;
            this.height = Number.isFinite(Number(height)) ? Number(height) : 0;
          }

          get top() { return Math.min(this.y, this.y + this.height); }
          get right() { return Math.max(this.x, this.x + this.width); }
          get bottom() { return Math.max(this.y, this.y + this.height); }
          get left() { return Math.min(this.x, this.x + this.width); }
          toJSON() {
            return {
              x: this.x,
              y: this.y,
              width: this.width,
              height: this.height,
              top: this.top,
              right: this.right,
              bottom: this.bottom,
              left: this.left
            };
          }

          static fromRect(other = {}) {
            return new DOMRectReadOnly(other.x, other.y, other.width, other.height);
          }

          get [Symbol.toStringTag]() { return 'DOMRectReadOnly'; }
        }

        class DOMRect extends DOMRectReadOnly {
          static fromRect(other = {}) {
            return new DOMRect(other.x, other.y, other.width, other.height);
          }

          get [Symbol.toStringTag]() { return 'DOMRect'; }
        }

        class DOMRectList {
          constructor(rects = []) {
            this._rects = Array.from(rects);
            for (let index = 0; index < this._rects.length; index++) {
              Object.defineProperty(this, index, {
                configurable: true,
                enumerable: true,
                value: this._rects[index]
              });
            }
          }

          get length() { return this._rects.length; }
          item(index) { return this._rects[Number(index)] || null; }
          [Symbol.iterator]() { return this._rects[Symbol.iterator](); }
          get [Symbol.toStringTag]() { return 'DOMRectList'; }
        }

        // Reduced-scope CaretPosition: offsetNode/offset identify the right
        // element (or its first Text descendant), not a glyph-precise character
        // offset -- litehtml exposes no per-glyph text metrics for that. offset
        // is always 0. See Document.caretPositionFromPoint (dom module) for the
        // hit-testing side.
        class CaretPosition {
          constructor(offsetNode, offset) {
            this._offsetNode = offsetNode;
            this._offset = offset;
          }

          get offsetNode() { return this._offsetNode; }
          get offset() { return this._offset; }

          getClientRect() {
            const node = this._offsetNode;
            if (!node) return null;
            const element = node.nodeType === 1 ? node : node.parentElement;
            return element && typeof element.getBoundingClientRect === 'function'
              ? element.getBoundingClientRect()
              : null;
          }

          get [Symbol.toStringTag]() { return 'CaretPosition'; }
        }

        function isCharacterDataNode(node) {
          return node != null && typeof node.data === 'string';
        }

        function nodeLength(node) {
          if (!node) return 0;
          // Any CharacterData (Text, Comment, CDATASection, ProcessingInstruction),
          // not just Text -- a Comment's "length" is its data length too.
          if (isCharacterDataNode(node)) return node.data.length;
          return node.childNodes.length;
        }

        function nodeIndexOf(node) {
          let index = 0;
          for (let sibling = node.previousSibling; sibling; sibling = sibling.previousSibling) index++;
          return index;
        }

        // Preorder "next node", unbounded (returns null past the last node in
        // the tree). Combined with a stop condition, this walks exactly the
        // nodes between two points without needing a dedicated tree-walker.
        function nextNodeInTreeOrder(node) {
          if (node.firstChild) return node.firstChild;
          let current = node;
          while (current) {
            if (current.nextSibling) return current.nextSibling;
            current = current.parentNode;
          }
          return null;
        }

        // The node immediately after `node`'s entire subtree ends, i.e. skips
        // over its descendants (unlike nextNodeInTreeOrder, which would visit
        // them). Used to bound a walk to "just this subtree".
        function nodeAfterSubtree(node) {
          let current = node;
          while (current) {
            if (current.nextSibling) return current.nextSibling;
            current = current.parentNode;
          }
          return null;
        }

        // https://dom.spec.whatwg.org/#concept-range-bp-position -- returns -1
        // if (nodeA, offsetA) is before (nodeB, offsetB), 0 if equal, 1 if after.
        // Built on the already-tested compareDocumentPosition() rather than a
        // second, parallel tree-order implementation.
        function comparePointPosition(nodeA, offsetA, nodeB, offsetB) {
          if (nodeA === nodeB) return offsetA < offsetB ? -1 : offsetA > offsetB ? 1 : 0;
          const CONTAINS = 8, CONTAINED_BY = 16, PRECEDING = 2;
          const position = nodeA.compareDocumentPosition(nodeB);
          if (position & CONTAINED_BY) {
            // nodeA is an ancestor of nodeB.
            let child = nodeB;
            while (child.parentNode !== nodeA) child = child.parentNode;
            return nodeIndexOf(child) < offsetA ? 1 : -1;
          }
          if (position & CONTAINS) {
            // nodeB is an ancestor of nodeA.
            let child = nodeA;
            while (child.parentNode !== nodeB) child = child.parentNode;
            return nodeIndexOf(child) < offsetB ? -1 : 1;
          }
          return (position & PRECEDING) ? 1 : -1;
        }

        // A node is "partially contained" in range if it is an inclusive
        // ancestor of exactly one of range's two boundary-point containers.
        function isPartiallyContained(node, range) {
          const startInside = node.contains(range._startContainer);
          const endInside = node.contains(range._endContainer);
          return startInside !== endInside;
        }

        // A node is "fully contained" if its entire content sits strictly
        // between range's boundary points.
        function isFullyContained(node, range) {
          return comparePointPosition(node, 0, range._startContainer, range._startOffset) > 0
            && comparePointPosition(node, nodeLength(node), range._endContainer, range._endOffset) < 0;
        }

        function commonAncestorContainerOf(range) {
          let node = range._startContainer;
          while (!node.contains(range._endContainer)) node = node.parentNode;
          return node;
        }

        // Shared by extractContents() (clone=false) and cloneContents() (clone=true):
        // https://dom.spec.whatwg.org/#concept-range-extract
        // https://dom.spec.whatwg.org/#concept-range-clone
        function rangeExtractOrClone(range, clone) {
          const fragment = document.createDocumentFragment();
          if (range.collapsed) return fragment;

          const originalStartNode = range._startContainer;
          const originalStartOffset = range._startOffset;
          const originalEndNode = range._endContainer;
          const originalEndOffset = range._endOffset;

          if (originalStartNode === originalEndNode && isCharacterDataNode(originalStartNode)) {
            const clonedNode = originalStartNode.cloneNode(false);
            clonedNode.data = originalStartNode.substringData(originalStartOffset, originalEndOffset - originalStartOffset);
            fragment.appendChild(clonedNode);
            if (!clone) originalStartNode.replaceData(originalStartOffset, originalEndOffset - originalStartOffset, '');
            return fragment;
          }

          const commonAncestor = commonAncestorContainerOf(range);

          let firstPartiallyContainedChild = null;
          if (!originalStartNode.contains(originalEndNode)) {
            for (const child of commonAncestor.childNodes) {
              if (isPartiallyContained(child, range)) { firstPartiallyContainedChild = child; break; }
            }
          }

          let lastPartiallyContainedChild = null;
          if (!originalEndNode.contains(originalStartNode)) {
            const children = [...commonAncestor.childNodes];
            for (let i = children.length - 1; i >= 0; i--) {
              if (isPartiallyContained(children[i], range)) { lastPartiallyContainedChild = children[i]; break; }
            }
          }

          const containedChildren = [...commonAncestor.childNodes].filter((child) => isFullyContained(child, range));
          if (containedChildren.some((child) => child.nodeType === 10)) {
            throw new DOMException('A DocumentType cannot be extracted or cloned.', 'HierarchyRequestError');
          }

          let newNode, newOffset;
          if (originalStartNode.contains(originalEndNode)) {
            newNode = originalStartNode;
            newOffset = originalStartOffset;
          } else {
            let referenceNode = originalStartNode;
            while (referenceNode.parentNode && !referenceNode.parentNode.contains(originalEndNode)) {
              referenceNode = referenceNode.parentNode;
            }
            newNode = referenceNode.parentNode;
            newOffset = nodeIndexOf(referenceNode) + 1;
          }

          if (firstPartiallyContainedChild && isCharacterDataNode(firstPartiallyContainedChild)) {
            const clonedNode = originalStartNode.cloneNode(false);
            clonedNode.data = originalStartNode.substringData(originalStartOffset, nodeLength(originalStartNode) - originalStartOffset);
            fragment.appendChild(clonedNode);
            if (!clone) originalStartNode.replaceData(originalStartOffset, nodeLength(originalStartNode) - originalStartOffset, '');
          } else if (firstPartiallyContainedChild) {
            const clonedChild = firstPartiallyContainedChild.cloneNode(false);
            fragment.appendChild(clonedChild);
            const subrange = new Range();
            subrange._startContainer = originalStartNode;
            subrange._startOffset = originalStartOffset;
            subrange._endContainer = firstPartiallyContainedChild;
            subrange._endOffset = nodeLength(firstPartiallyContainedChild);
            clonedChild.appendChild(rangeExtractOrClone(subrange, clone));
          }

          for (const child of containedChildren) {
            fragment.appendChild(clone ? child.cloneNode(true) : child);
          }

          if (lastPartiallyContainedChild && isCharacterDataNode(lastPartiallyContainedChild)) {
            const clonedNode = originalEndNode.cloneNode(false);
            clonedNode.data = originalEndNode.substringData(0, originalEndOffset);
            fragment.appendChild(clonedNode);
            if (!clone) originalEndNode.replaceData(0, originalEndOffset, '');
          } else if (lastPartiallyContainedChild) {
            const clonedChild = lastPartiallyContainedChild.cloneNode(false);
            fragment.appendChild(clonedChild);
            const subrange = new Range();
            subrange._startContainer = lastPartiallyContainedChild;
            subrange._startOffset = 0;
            subrange._endContainer = originalEndNode;
            subrange._endOffset = originalEndOffset;
            clonedChild.appendChild(rangeExtractOrClone(subrange, clone));
          }

          if (!clone) {
            range._startContainer = newNode;
            range._startOffset = newOffset;
            range._endContainer = newNode;
            range._endOffset = newOffset;
          }

          return fragment;
        }

        class Range {
          constructor(ownerDocument = document) {
            this._startContainer = ownerDocument;
            this._startOffset = 0;
            this._endContainer = ownerDocument;
            this._endOffset = 0;
          }

          get startContainer() { return this._startContainer; }
          get startOffset() { return this._startOffset; }
          get endContainer() { return this._endContainer; }
          get endOffset() { return this._endOffset; }
          get collapsed() { return this._startContainer === this._endContainer && this._startOffset === this._endOffset; }
          get commonAncestorContainer() { return commonAncestorContainerOf(this); }

          setStart(node, offset) {
            assertRangeBoundary(node, offset);
            this._startContainer = node;
            this._startOffset = Number(offset);
          }

          setEnd(node, offset) {
            assertRangeBoundary(node, offset);
            this._endContainer = node;
            this._endOffset = Number(offset);
          }

          setStartBefore(node) {
            if (!node.parentNode) throw new DOMException('Node has no parent.', 'InvalidNodeTypeError');
            this.setStart(node.parentNode, [...node.parentNode.childNodes].indexOf(node));
          }

          setStartAfter(node) {
            if (!node.parentNode) throw new DOMException('Node has no parent.', 'InvalidNodeTypeError');
            this.setStart(node.parentNode, [...node.parentNode.childNodes].indexOf(node) + 1);
          }

          setEndBefore(node) {
            if (!node.parentNode) throw new DOMException('Node has no parent.', 'InvalidNodeTypeError');
            this.setEnd(node.parentNode, [...node.parentNode.childNodes].indexOf(node));
          }

          setEndAfter(node) {
            if (!node.parentNode) throw new DOMException('Node has no parent.', 'InvalidNodeTypeError');
            this.setEnd(node.parentNode, [...node.parentNode.childNodes].indexOf(node) + 1);
          }

          selectNode(node) {
            if (!node.parentNode) throw new DOMException('Node has no parent.', 'InvalidNodeTypeError');
            const index = [...node.parentNode.childNodes].indexOf(node);
            this._startContainer = node.parentNode;
            this._startOffset = index;
            this._endContainer = node.parentNode;
            this._endOffset = index + 1;
          }

          selectNodeContents(node) {
            this._startContainer = node;
            this._startOffset = 0;
            this._endContainer = node;
            this._endOffset = nodeLength(node);
          }

          collapse(toStart = false) {
            if (toStart) {
              this._endContainer = this._startContainer;
              this._endOffset = this._startOffset;
            } else {
              this._startContainer = this._endContainer;
              this._startOffset = this._endOffset;
            }
          }

          cloneRange() {
            const range = new Range();
            range._startContainer = this._startContainer;
            range._startOffset = this._startOffset;
            range._endContainer = this._endContainer;
            range._endOffset = this._endOffset;
            return range;
          }

          detach() {}
          getBoundingClientRect() { return new DOMRect(0, 0, 0, 0); }
          getClientRects() {
            if (this._startContainer && typeof this._startContainer.getClientRects === 'function') {
              return this._startContainer.getClientRects();
            }
            return new DOMRectList([]);
          }
          // https://dom.spec.whatwg.org/#dom-range-stringifier -- concatenates
          // partial Text data from the boundary containers plus the full data
          // of every fully-contained Text node in between, in tree order. Not
          // simply this._startContainer.textContent: that would include text
          // from outside the range entirely for a multi-node range.
          toString() {
            const start = this._startContainer;
            const end = this._endContainer;
            if (start === end && start.nodeType === 3) {
              return start.data.slice(this._startOffset, this._endOffset);
            }
            let result = '';
            if (start.nodeType === 3) result += start.data.slice(this._startOffset);
            const boundary = nodeAfterSubtree(end);
            for (let node = start; node && node !== boundary; node = nextNodeInTreeOrder(node)) {
              if (node.nodeType === 3 && isFullyContained(node, this)) result += node.data;
            }
            if (end.nodeType === 3) result += end.data.slice(0, this._endOffset);
            return result;
          }

          createContextualFragment(html) {
            return fragmentFromHTML(html);
          }

          compareBoundaryPoints(how, sourceRange) {
            if (how !== 0 && how !== 1 && how !== 2 && how !== 3) {
              throw new DOMException(
                "The comparison method provided must be one of 'START_TO_START', 'START_TO_END', 'END_TO_END', or 'END_TO_START'.",
                'NotSupportedError');
            }
            if (this._startContainer.getRootNode() !== sourceRange._startContainer.getRootNode()) {
              throw new DOMException('The two Ranges are not in the same tree.', 'WrongDocumentError');
            }
            let thisNode, thisOffset, otherNode, otherOffset;
            if (how === 0) { thisNode = this._startContainer; thisOffset = this._startOffset; otherNode = sourceRange._startContainer; otherOffset = sourceRange._startOffset; }
            else if (how === 1) { thisNode = this._endContainer; thisOffset = this._endOffset; otherNode = sourceRange._startContainer; otherOffset = sourceRange._startOffset; }
            else if (how === 2) { thisNode = this._endContainer; thisOffset = this._endOffset; otherNode = sourceRange._endContainer; otherOffset = sourceRange._endOffset; }
            else { thisNode = this._startContainer; thisOffset = this._startOffset; otherNode = sourceRange._endContainer; otherOffset = sourceRange._endOffset; }
            return comparePointPosition(thisNode, thisOffset, otherNode, otherOffset);
          }

          comparePoint(node, offset) {
            if (node.getRootNode() !== this._startContainer.getRootNode()) {
              throw new DOMException('The given Node and the Range are not in the same tree.', 'WrongDocumentError');
            }
            if (node.nodeType === 10) throw new DOMException('DocumentType is not allowed.', 'InvalidNodeTypeError');
            const numericOffset = Number(offset) >>> 0;
            if (numericOffset > nodeLength(node)) {
              throw new DOMException("The offset is larger than the node's length.", 'IndexSizeError');
            }
            if (comparePointPosition(node, numericOffset, this._startContainer, this._startOffset) < 0) return -1;
            if (comparePointPosition(node, numericOffset, this._endContainer, this._endOffset) > 0) return 1;
            return 0;
          }

          isPointInRange(node, offset) {
            if (node.getRootNode() !== this._startContainer.getRootNode()) {
              throw new DOMException('The given Node and the Range are not in the same tree.', 'WrongDocumentError');
            }
            if (node.nodeType === 10) throw new DOMException('DocumentType is not allowed.', 'InvalidNodeTypeError');
            const numericOffset = Number(offset) >>> 0;
            if (numericOffset > nodeLength(node)) {
              throw new DOMException("The offset is larger than the node's length.", 'IndexSizeError');
            }
            if (comparePointPosition(node, numericOffset, this._startContainer, this._startOffset) < 0) return false;
            if (comparePointPosition(node, numericOffset, this._endContainer, this._endOffset) > 0) return false;
            return true;
          }

          intersectsNode(node) {
            if (node.getRootNode() !== this._startContainer.getRootNode()) return false;
            const parent = node.parentNode;
            if (!parent) return true;
            const offset = nodeIndexOf(node);
            return comparePointPosition(parent, offset, this._endContainer, this._endOffset) < 0
              && comparePointPosition(parent, offset + 1, this._startContainer, this._startOffset) > 0;
          }

          cloneContents() { return rangeExtractOrClone(this, true); }
          extractContents() { return rangeExtractOrClone(this, false); }

          deleteContents() {
            if (this.collapsed) return;
            const originalStartNode = this._startContainer;
            const originalStartOffset = this._startOffset;
            const originalEndNode = this._endContainer;
            const originalEndOffset = this._endOffset;

            if (originalStartNode === originalEndNode && isCharacterDataNode(originalStartNode)) {
              originalStartNode.replaceData(originalStartOffset, originalEndOffset - originalStartOffset, '');
              return;
            }

            // Collect the topmost fully-contained nodes (a node whose parent is
            // also fully contained is skipped -- removing the parent already
            // removes it, and it must not be listed twice for removal).
            const nodesToRemove = [];
            const endBoundary = nodeAfterSubtree(originalEndNode);
            for (let node = originalStartNode; node && node !== endBoundary; node = nextNodeInTreeOrder(node)) {
              if (isFullyContained(node, this) && !(node.parentNode && isFullyContained(node.parentNode, this))) {
                nodesToRemove.push(node);
              }
            }

            let newNode, newOffset;
            if (originalStartNode.contains(originalEndNode)) {
              newNode = originalStartNode;
              newOffset = originalStartOffset;
            } else {
              let referenceNode = originalStartNode;
              while (referenceNode.parentNode && !referenceNode.parentNode.contains(originalEndNode)) {
                referenceNode = referenceNode.parentNode;
              }
              newNode = referenceNode.parentNode;
              newOffset = nodeIndexOf(referenceNode) + 1;
            }

            if (isCharacterDataNode(originalStartNode)) {
              originalStartNode.replaceData(originalStartOffset, nodeLength(originalStartNode) - originalStartOffset, '');
            }
            for (const node of nodesToRemove) node.parentNode.removeChild(node);
            if (isCharacterDataNode(originalEndNode)) {
              originalEndNode.replaceData(0, originalEndOffset, '');
            }

            this._startContainer = newNode;
            this._startOffset = newOffset;
            this._endContainer = newNode;
            this._endOffset = newOffset;
          }

          insertNode(node) {
            const startNode = this._startContainer;
            const startOffset = this._startOffset;
            if (startNode.nodeType === 7 || startNode.nodeType === 8
              || (startNode.nodeType === 3 && !startNode.parentNode)
              || node === startNode) {
              throw new DOMException('Invalid start node for insertNode.', 'HierarchyRequestError');
            }

            let referenceNode = startNode.nodeType === 3 ? startNode : ([...startNode.childNodes][startOffset] || null);
            const parent = referenceNode ? referenceNode.parentNode : startNode;

            if (startNode.nodeType === 3) referenceNode = startNode.splitText(startOffset);
            if (node === referenceNode) referenceNode = referenceNode.nextSibling;
            if (node.parentNode) node.parentNode.removeChild(node);

            let newOffset = referenceNode ? nodeIndexOf(referenceNode) : nodeLength(parent);
            newOffset += node.nodeType === 11 ? nodeLength(node) : 1;

            parent.insertBefore(node, referenceNode);

            if (this.collapsed) {
              this._endContainer = parent;
              this._endOffset = newOffset;
            }
          }

          surroundContents(newParent) {
            // Walk the common ancestor's whole subtree: any non-Text node that
            // is only partially inside the range makes a well-formed wrapping
            // impossible (it would have to be split, which surroundContents()
            // never does for anything but the boundary Text nodes).
            const commonAncestor = this.commonAncestorContainer;
            const boundary = nodeAfterSubtree(commonAncestor);
            for (let node = commonAncestor; node && node !== boundary; node = nextNodeInTreeOrder(node)) {
              if (node.nodeType !== 3 && isPartiallyContained(node, this)) {
                throw new DOMException('The Range partially contains a non-Text node.', 'InvalidStateError');
              }
            }
            if (newParent.nodeType === 9 || newParent.nodeType === 10 || newParent.nodeType === 11) {
              throw new DOMException('Invalid element type for surroundContents.', 'InvalidNodeTypeError');
            }
            const fragment = this.extractContents();
            while (newParent.firstChild) newParent.removeChild(newParent.firstChild);
            this.insertNode(newParent);
            newParent.appendChild(fragment);
            this.selectNode(newParent);
          }
        }

        defineValue(Range, 'START_TO_START', 0, true);
        defineValue(Range, 'START_TO_END', 1, true);
        defineValue(Range, 'END_TO_END', 2, true);
        defineValue(Range, 'END_TO_START', 3, true);

        // A StaticRange just records boundary points; unlike Range it does not
        // track tree mutations, so it needs no live bookkeeping.
        class StaticRange {
          constructor(init) {
            if (init === null || typeof init !== 'object') {
              throw new TypeError("Failed to construct 'StaticRange': parameter 1 is not of type 'StaticRangeInit'.");
            }
            for (const key of ['startContainer', 'startOffset', 'endContainer', 'endOffset']) {
              if (!(key in init)) {
                throw new TypeError(`Failed to construct 'StaticRange': required member ${key} is undefined.`);
              }
            }
            // DocumentType (10) and Attr (2) cannot be boundary containers.
            for (const container of [init.startContainer, init.endContainer]) {
              if (!container || typeof container.nodeType !== 'number') {
                throw new TypeError("Failed to construct 'StaticRange': member is not of type 'Node'.");
              }
              if (container.nodeType === 10 || container.nodeType === 2) {
                throw new DOMException('The supplied node is incorrect.', 'InvalidNodeTypeError');
              }
            }
            this._startContainer = init.startContainer;
            this._startOffset = Number(init.startOffset) >>> 0;
            this._endContainer = init.endContainer;
            this._endOffset = Number(init.endOffset) >>> 0;
          }

          get startContainer() { return this._startContainer; }
          get startOffset() { return this._startOffset; }
          get endContainer() { return this._endContainer; }
          get endOffset() { return this._endOffset; }
          get collapsed() {
            return this._startContainer === this._endContainer && this._startOffset === this._endOffset;
          }

          get [Symbol.toStringTag]() { return 'StaticRange'; }
        }

        function assertRangeBoundary(node, offset) {
          if (!node || typeof node.nodeType !== 'number') throw new TypeError('Boundary container must be a Node');
          const numericOffset = Number(offset);
          if (!Number.isInteger(numericOffset) || numericOffset < 0 || numericOffset > nodeLength(node)) {
            throw new DOMException('Boundary offset is outside the node.', 'IndexSizeError');
          }
        }

        class Selection {
          constructor() {
            this._ranges = [];
          }

          get rangeCount() { return this._ranges.length; }
          get isCollapsed() { return this.rangeCount === 0 || this._ranges[0].collapsed; }
          get type() { return this.rangeCount === 0 ? 'None' : (this.isCollapsed ? 'Caret' : 'Range'); }
          get anchorNode() { return this.rangeCount ? this._ranges[0].startContainer : null; }
          get anchorOffset() { return this.rangeCount ? this._ranges[0].startOffset : 0; }
          get focusNode() { return this.rangeCount ? this._ranges[0].endContainer : null; }
          get focusOffset() { return this.rangeCount ? this._ranges[0].endOffset : 0; }

          getRangeAt(index) {
            if (index < 0 || index >= this._ranges.length) throw new DOMException('Range index is invalid.', 'IndexSizeError');
            return this._ranges[index];
          }

          removeAllRanges() { this._ranges = []; }
          addRange(range) {
            if (!(range instanceof Range)) throw new TypeError('addRange expects Range');
            this._ranges = [range];
          }
          removeRange(range) { this._ranges = this._ranges.filter((candidate) => candidate !== range); }
          collapse(node, offset = 0) {
            const range = new Range();
            range.setStart(node, offset);
            range.collapse(true);
            this._ranges = [range];
          }
          selectAllChildren(node) {
            const range = new Range();
            range.selectNodeContents(node);
            this._ranges = [range];
          }
          toString() { return this.rangeCount ? this._ranges[0].toString() : ''; }
        }

        const selection = new Selection();

        // WebIDL USVString conversion: a lone surrogate cannot round-trip through
        // UTF-8, so it is replaced with U+FFFD wherever a spec algorithm treats a
        // value as USVString (URLSearchParams names/values, record keys/values).
        function toUSVString(value) {
          const str = String(value);
          let result = '';
          for (let i = 0; i < str.length; i++) {
            const code = str.charCodeAt(i);
            if (code >= 0xd800 && code <= 0xdbff) {
              const next = str.charCodeAt(i + 1);
              if (next >= 0xdc00 && next <= 0xdfff) {
                result += str[i] + str[i + 1];
                i++;
              } else {
                result += '\uFFFD';
              }
            } else if (code >= 0xdc00 && code <= 0xdfff) {
              result += '\uFFFD';
            } else {
              result += str[i];
            }
          }
          return result;
        }

        function usv(value) { return toUSVString(value); }

        function utf8EncodeScalar(text) {
          const bytes = [];
          for (const char of toUSVString(text)) {
            const code = char.codePointAt(0);
            if (code <= 0x7f) {
              bytes.push(code);
            } else if (code <= 0x7ff) {
              bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
            } else if (code <= 0xffff) {
              bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
            } else {
              bytes.push(
                0xf0 | (code >> 18),
                0x80 | ((code >> 12) & 0x3f),
                0x80 | ((code >> 6) & 0x3f),
                0x80 | (code & 0x3f)
              );
            }
          }
          return new Uint8Array(bytes);
        }

        // WHATWG Encoding Standard "UTF-8 decoder": a malformed or incomplete
        // sequence yields one U+FFFD, and a continuation byte that turns out to
        // be invalid is re-examined as a fresh lead byte rather than consumed.
        function utf8DecodeReplacement(bytes, { fatal = false } = {}) {
          let out = '';
          let codePoint = 0;
          let bytesSeen = 0;
          let bytesNeeded = 0;
          let lowerBoundary = 0x80;
          let upperBoundary = 0xbf;
          for (let i = 0; i < bytes.length; i++) {
            const byte = bytes[i];
            if (bytesNeeded === 0) {
              if (byte <= 0x7f) {
                out += String.fromCharCode(byte);
              } else if (byte >= 0xc2 && byte <= 0xdf) {
                bytesNeeded = 1;
                codePoint = byte & 0x1f;
              } else if (byte >= 0xe0 && byte <= 0xef) {
                if (byte === 0xe0) lowerBoundary = 0xa0;
                else if (byte === 0xed) upperBoundary = 0x9f;
                bytesNeeded = 2;
                codePoint = byte & 0x0f;
              } else if (byte >= 0xf0 && byte <= 0xf4) {
                if (byte === 0xf0) lowerBoundary = 0x90;
                else if (byte === 0xf4) upperBoundary = 0x8f;
                bytesNeeded = 3;
                codePoint = byte & 0x07;
              } else {
                if (fatal) throw new TypeError('Invalid UTF-8 data');
                out += '\uFFFD';
              }
              continue;
            }
            if (byte < lowerBoundary || byte > upperBoundary) {
              codePoint = 0;
              bytesNeeded = 0;
              bytesSeen = 0;
              lowerBoundary = 0x80;
              upperBoundary = 0xbf;
              if (fatal) throw new TypeError('Invalid UTF-8 data');
              out += '\uFFFD';
              i--;
              continue;
            }
            lowerBoundary = 0x80;
            upperBoundary = 0xbf;
            codePoint = (codePoint << 6) | (byte & 0x3f);
            bytesSeen++;
            if (bytesSeen !== bytesNeeded) continue;
            out += String.fromCodePoint(codePoint);
            codePoint = 0;
            bytesNeeded = 0;
            bytesSeen = 0;
          }
          if (bytesNeeded !== 0) {
            if (fatal) throw new TypeError('Invalid UTF-8 data');
            out += '\uFFFD';
          }
          return out;
        }

        function isHexDigitByte(byte) {
          return (byte >= 0x30 && byte <= 0x39) || (byte >= 0x41 && byte <= 0x46) || (byte >= 0x61 && byte <= 0x66);
        }
        function hexByteValue(byte) {
          if (byte <= 0x39) return byte - 0x30;
          if (byte <= 0x46) return byte - 0x41 + 10;
          return byte - 0x61 + 10;
        }
        // Spec "percent-decode": only a well-formed "%XX" is consumed; a lone
        // '%' not followed by two hex digits passes through unchanged instead of
        // throwing, unlike decodeURIComponent.
        function percentDecodeBytes(bytes) {
          const out = [];
          for (let i = 0; i < bytes.length; i++) {
            const byte = bytes[i];
            if (byte === 0x25 && i + 2 < bytes.length && isHexDigitByte(bytes[i + 1]) && isHexDigitByte(bytes[i + 2])) {
              out.push((hexByteValue(bytes[i + 1]) << 4) | hexByteValue(bytes[i + 2]));
              i += 2;
            } else {
              out.push(byte);
            }
          }
          return new Uint8Array(out);
        }

        function replacePlusWithSpace(bytes) {
          const out = new Uint8Array(bytes.length);
          for (let i = 0; i < bytes.length; i++) out[i] = bytes[i] === 0x2b ? 0x20 : bytes[i];
          return out;
        }

        function isFormUrlencodedSafeByte(byte) {
          return (byte >= 0x30 && byte <= 0x39) || (byte >= 0x41 && byte <= 0x5a) || (byte >= 0x61 && byte <= 0x7a)
            || byte === 0x2a || byte === 0x2d || byte === 0x2e || byte === 0x5f;
        }

        function encodeFormComponent(value) {
          const bytes = utf8EncodeScalar(value);
          let out = '';
          for (const byte of bytes) {
            if (byte === 0x20) out += '+';
            else if (isFormUrlencodedSafeByte(byte)) out += String.fromCharCode(byte);
            else out += `%${byte.toString(16).toUpperCase().padStart(2, '0')}`;
          }
          return out;
        }

        // Spec "application/x-www-form-urlencoded parser": works on bytes so a
        // malformed percent-escape or invalid UTF-8 byte never throws — it
        // degrades to a literal '%' or a U+FFFD, per spec.
        function parseUrlencodedPairs(input) {
          const bytes = utf8EncodeScalar(input);
          const sequences = [];
          let start = 0;
          for (let i = 0; i <= bytes.length; i++) {
            if (i === bytes.length || bytes[i] === 0x26) {
              sequences.push(bytes.subarray(start, i));
              start = i + 1;
            }
          }
          const output = [];
          for (const seq of sequences) {
            if (seq.length === 0) continue;
            let eq = -1;
            for (let i = 0; i < seq.length; i++) {
              if (seq[i] === 0x3d) { eq = i; break; }
            }
            const nameBytes = eq < 0 ? seq : seq.subarray(0, eq);
            const valueBytes = eq < 0 ? new Uint8Array(0) : seq.subarray(eq + 1);
            const name = utf8DecodeReplacement(percentDecodeBytes(replacePlusWithSpace(nameBytes)));
            const value = utf8DecodeReplacement(percentDecodeBytes(replacePlusWithSpace(valueBytes)));
            output.push([name, value]);
          }
          return output;
        }

        // Same live-list semantics as forEach(): mutating the list mid-iteration
        // (e.g. delete()) shifts later entries, and the walk must observe that
        // rather than iterating a snapshot taken at call time.
        function urlSearchParamsIterator(target, mapEntry) {
          let index = 0;
          return {
            [Symbol.iterator]() { return this; },
            next() {
              if (index >= target._entries.length) return { value: undefined, done: true };
              const entry = target._entries[index];
              index += 1;
              return { value: mapEntry(entry), done: false };
            }
          };
        }

        class URLSearchParams {
          constructor(init = undefined) {
            this._entries = [];
            // Optional callback invoked after mutations so an owning URL can keep
            // its search/href in sync with a live searchParams object.
            this._onChange = null;
            if (init == null) return;
            // WebIDL's Type(V) is "Object" for both plain objects and callable
            // objects (functions); `typeof` alone would misroute a function
            // (e.g. `new URLSearchParams(DOMException)`) into the USVString
            // fallback below, stringifying its source text instead of reading
            // its own enumerable properties as a record.
            const isObjectLike = (typeof init === 'object' && init !== null) || typeof init === 'function';
            if (typeof init === 'string') {
              const query = init.startsWith('?') ? init.slice(1) : init;
              this._entries = parseUrlencodedPairs(query);
            } else if (isObjectLike && typeof init[Symbol.iterator] === 'function') {
              // Deliberately not special-cased for `init instanceof
              // URLSearchParams`: a page can override `init[Symbol.iterator]`,
              // and per spec that override must be honored, not bypassed by a
              // fast path that reads _entries directly.
              for (const pair of init) {
                if (pair == null || typeof pair[Symbol.iterator] !== 'function') {
                  throw new TypeError('URLSearchParams entry must be an iterable pair');
                }
                const items = [...pair];
                if (items.length !== 2) throw new TypeError('URLSearchParams entry must be a pair of length 2');
                this.append(items[0], items[1]);
              }
            } else if (isObjectLike) {
              // record<USVString, USVString>: duplicate keys (including two keys
              // that only collide after USVString conversion) collapse to their
              // last value but keep their first position, like Map#set.
              const map = new Map();
              for (const key of Object.keys(init)) map.set(usv(key), usv(init[key]));
              this._entries = [...map.entries()];
            } else {
              this._entries = parseUrlencodedPairs(String(init));
            }
          }

          _notify() { if (typeof this._onChange === 'function') this._onChange(this.toString()); }
          append(name, value) { this._entries.push([usv(name), usv(value)]); this._notify(); }
          delete(name, value = undefined) {
            name = usv(name);
            if (value === undefined) {
              this._entries = this._entries.filter((entry) => entry[0] !== name);
            } else {
              value = usv(value);
              this._entries = this._entries.filter((entry) => !(entry[0] === name && entry[1] === value));
            }
            this._notify();
          }
          get(name) {
            name = usv(name);
            const found = this._entries.find((entry) => entry[0] === name);
            return found ? found[1] : null;
          }
          getAll(name) {
            name = usv(name);
            return this._entries.filter((entry) => entry[0] === name).map((entry) => entry[1]);
          }
          has(name, value = undefined) {
            name = usv(name);
            if (value === undefined) return this._entries.some((entry) => entry[0] === name);
            value = usv(value);
            return this._entries.some((entry) => entry[0] === name && entry[1] === value);
          }
          set(name, value) {
            name = usv(name);
            value = usv(value);
            let replaced = false;
            const out = [];
            for (const entry of this._entries) {
              if (entry[0] === name) {
                if (!replaced) {
                  out.push([name, value]);
                  replaced = true;
                }
              } else {
                out.push(entry);
              }
            }
            if (!replaced) out.push([name, value]);
            this._entries = out;
            this._notify();
          }
          sort() {
            // WHATWG sorts by UTF-16 code units (stably), not locale collation, so
            // canonical query strings (OAuth1/SigV4 signing) come out consistent.
            this._entries.sort((left, right) => (left[0] < right[0] ? -1 : left[0] > right[0] ? 1 : 0));
            this._notify();
          }
          forEach(callback, thisArg = undefined) {
            let index = 0;
            while (index < this._entries.length) {
              const entry = this._entries[index];
              index += 1;
              callback.call(thisArg, entry[1], entry[0], this);
            }
          }
          keys() { return urlSearchParamsIterator(this, (entry) => entry[0]); }
          values() { return urlSearchParamsIterator(this, (entry) => entry[1]); }
          entries() { return urlSearchParamsIterator(this, (entry) => [entry[0], entry[1]]); }
          get size() { return this._entries.length; }
          toString() {
            return this._entries.map(([name, value]) => `${encodeFormComponent(name)}=${encodeFormComponent(value)}`).join('&');
          }
          [Symbol.iterator]() { return this.entries(); }
        }

        // The six schemes the URL Standard calls "special": their host is a
        // domain (subject to IDNA/Punycode) rather than an opaque, verbatim
        // string, and an empty or IDNA-rejected host is a parse failure.
        const SPECIAL_SCHEMES = new Set(['http:', 'https:', 'ws:', 'wss:', 'ftp:', 'file:']);

        function percentDecodeString(text) {
          return utf8DecodeReplacement(percentDecodeBytes(utf8EncodeScalar(text)));
        }

        // Spec "forbidden domain code point": a WHATWG URL Standard overlay on
        // top of UTS46/IDNA — these characters are rejected by host parsing
        // even though IDNA mapping itself would happily pass most of them
        // through (e.g. U+0020 SPACE is not disallowed by UTS46 mapping).
        const FORBIDDEN_HOST_CODE_POINTS = new Set([' ', '#', '/', ':', '<', '>', '?', '@', '[', '\\', ']', '^', '|']);
        function hasForbiddenDomainCodePoint(text) {
          for (const ch of text) {
            const code = ch.codePointAt(0);
            if (code <= 0x1f || code === 0x25 || code === 0x7f) return true; // C0 control, '%', DELETE
            if (FORBIDDEN_HOST_CODE_POINTS.has(ch)) return true;
          }
          return false;
        }

        // WHATWG "domain to ASCII", always with beStrict=false (the URL
        // Standard never invokes the strict/STD3 variant from host parsing).
        // Returns null on failure (disallowed code point, empty label, bad
        // hyphens, ...) rather than throwing, so callers decide whether that
        // means a hard parse failure (constructor/href) or a silent no-op
        // (the hostname/host setters).
        function domainToAscii(hostname) {
          const decoded = percentDecodeString(hostname);
          if (decoded === '' || hasForbiddenDomainCodePoint(decoded)) return null;
          const ascii = typeof host.domainToAscii === 'function' ? host.domainToAscii(decoded) : decoded.toLowerCase();
          if (typeof ascii !== 'string' || ascii === '' || hasForbiddenDomainCodePoint(ascii)) return null;
          return ascii;
        }

        function parseURL(input, base = undefined) {
          let raw = String(input).trim();
          if (base !== undefined && !/^[A-Za-z][A-Za-z0-9+.-]*:/.test(raw)) {
            raw = resolveURLAgainstBase(raw, base);
          }
          const match = /^([A-Za-z][A-Za-z0-9+.-]*:)(?:\/\/([^/?#]*))?([^?#]*)(\?[^#]*)?(#.*)?$/.exec(raw);
          if (!match) throw new TypeError(`Invalid URL: ${input}`);
          const protocol = match[1].toLowerCase();
          const authority = match[2] || '';
          let username = '';
          let password = '';
          let authorityHost = authority;
          const at = authority.lastIndexOf('@');
          if (at >= 0) {
            const auth = authority.slice(0, at);
            authorityHost = authority.slice(at + 1);
            const colon = auth.indexOf(':');
            username = decodeURIComponent(colon < 0 ? auth : auth.slice(0, colon));
            password = colon < 0 ? '' : decodeURIComponent(auth.slice(colon + 1));
          }
          const hostColon = authorityHost.lastIndexOf(':');
          let hostname = hostColon > 0 ? authorityHost.slice(0, hostColon) : authorityHost;
          const port = hostColon > 0 ? authorityHost.slice(hostColon + 1) : '';
          if (SPECIAL_SCHEMES.has(protocol)) {
            const ascii = domainToAscii(hostname);
            if (ascii === null) throw new TypeError(`Invalid URL: invalid host "${hostname}"`);
            hostname = ascii;
          }
          return {
            protocol,
            username,
            password,
            hostname,
            port,
            pathname: match[3] || '/',
            search: match[4] || '',
            hash: match[5] || ''
          };
        }

        function resolveURLAgainstBase(input, base) {
          const baseParts = typeof base === 'string' ? parseURL(base) : base._parts;
          if (input.startsWith('//')) return `${baseParts.protocol}${input}`;
          if (input.startsWith('/')) return `${baseParts.protocol}//${buildHost(baseParts)}${input}`;
          if (input.startsWith('?')) return `${baseParts.protocol}//${buildHost(baseParts)}${baseParts.pathname}${input}`;
          if (input.startsWith('#')) return `${baseParts.protocol}//${buildHost(baseParts)}${baseParts.pathname}${baseParts.search}${input}`;
          const basePath = baseParts.pathname || '/';
          const dir = basePath.slice(0, basePath.lastIndexOf('/') + 1);
          return `${baseParts.protocol}//${buildHost(baseParts)}${normalizePath(dir + input)}`;
        }

        function normalizePath(path) {
          const out = [];
          for (const segment of path.split('/')) {
            if (segment === '..') out.pop();
            else if (segment !== '.') out.push(segment);
          }
          return out.join('/') || '/';
        }

        function buildHost(parts) {
          return parts.port ? `${parts.hostname}:${parts.port}` : parts.hostname;
        }

        class URL {
          constructor(url, base = undefined) {
            this._parts = parseURL(url, base);
            this._searchParams = null;
          }

          get href() {
            // href carries userinfo (user[:password]@) when present; origin never
            // does (kept userinfo-free below, per the URL spec).
            const parts = this._parts;
            const credentials = parts.username || parts.password
              ? `${parts.username}${parts.password ? `:${parts.password}` : ''}@`
              : '';
            return `${parts.protocol}//${credentials}${this.host}${this.pathname}${this.search}${this.hash}`;
          }
          set href(value) { this._parts = parseURL(value); this._syncSearchParams(); }
          get origin() { return `${this.protocol}//${this.host}`; }
          get protocol() { return this._parts.protocol; }
          set protocol(value) { this._parts.protocol = String(value).replace(/:?$/, ':').toLowerCase(); }
          get username() { return this._parts.username; }
          set username(value) { this._parts.username = String(value); }
          get password() { return this._parts.password; }
          set password(value) { this._parts.password = String(value); }
          get host() { return buildHost(this._parts); }
          set host(value) {
            const text = String(value);
            const index = text.lastIndexOf(':');
            const candidateHostname = index > 0 ? text.slice(0, index) : text;
            const candidatePort = index > 0 ? text.slice(index + 1) : '';
            if (SPECIAL_SCHEMES.has(this._parts.protocol)) {
              // Per spec, a rejected host leaves both host and port untouched
              // rather than throwing: setters fail silently, unlike the
              // constructor.
              const ascii = domainToAscii(candidateHostname);
              if (ascii === null) return;
              this._parts.hostname = ascii;
              this._parts.port = candidatePort;
              return;
            }
            this._parts.hostname = candidateHostname;
            this._parts.port = candidatePort;
          }
          get hostname() { return this._parts.hostname; }
          set hostname(value) {
            const text = String(value);
            if (SPECIAL_SCHEMES.has(this._parts.protocol)) {
              const ascii = domainToAscii(text);
              if (ascii === null) return;
              this._parts.hostname = ascii;
              return;
            }
            this._parts.hostname = text;
          }
          get port() { return this._parts.port; }
          set port(value) { this._parts.port = String(value); }
          get pathname() { return this._parts.pathname; }
          set pathname(value) { this._parts.pathname = String(value).startsWith('/') ? String(value) : `/${value}`; }
          get search() { return this._parts.search; }
          set search(value) {
            const text = String(value);
            this._parts.search = text && !text.startsWith('?') ? `?${text}` : text;
            this._syncSearchParams();
          }
          get hash() { return this._parts.hash; }
          set hash(value) {
            const text = String(value);
            this._parts.hash = text && !text.startsWith('#') ? `#${text}` : text;
          }
          get searchParams() {
            if (!this._searchParams) {
              this._searchParams = new URLSearchParams(this.search);
              // Keep the URL's search/href in sync with live mutations.
              this._searchParams._onChange = (serialized) => {
                this._parts.search = serialized ? `?${serialized}` : '';
              };
            }
            return this._searchParams;
          }
          // Replaces the existing searchParams object's entries in place so its
          // identity (and any live for-of iterator over it) survives a `search`
          // or `href` assignment, instead of detaching it via reassignment.
          _syncSearchParams() {
            if (!this._searchParams) return;
            const query = this.search.startsWith('?') ? this.search.slice(1) : this.search;
            this._searchParams._entries = parseUrlencodedPairs(query);
          }
          toString() { return this.href; }
          toJSON() { return this.href; }
          static canParse(url, base = undefined) {
            try { new URL(url, base); return true; } catch (_error) { return false; }
          }
          static parse(url, base = undefined) {
            try { return new URL(url, base); } catch (_error) { return null; }
          }
        }

        class TextEncoder {
          get encoding() { return 'utf-8'; }

          encode(input = '') {
            return utf8EncodeScalar(String(input));
          }

          encodeInto(source, destination) {
            if (!(destination instanceof Uint8Array)) throw new TypeError('encodeInto destination must be Uint8Array');
            let read = 0;
            let written = 0;
            for (const char of String(source)) {
              const bytes = this.encode(char);
              if (written + bytes.length > destination.length) break;
              destination.set(bytes, written);
              written += bytes.length;
              read += char.length;
            }
            return { read, written };
          }
        }

        // A handful of legacy WHATWG labels Lexbor's own table does not list
        // (it is complete for every label WPT's textdecoder-labels.any.js
        // otherwise exercises). Mapped to a label Lexbor does recognize
        // rather than patching Lexbor upstream for nine aliases.
        const LEGACY_ENCODING_LABEL_ALIASES = {
          'unicode11utf8': 'utf-8',
          'unicode20utf8': 'utf-8',
          'x-unicode20utf8': 'utf-8',
          'unicodefffe': 'utf-16be',
          'csunicode': 'utf-16le',
          'iso-10646-ucs-2': 'utf-16le',
          'ucs-2': 'utf-16le',
          'unicode': 'utf-16le',
          'unicodefeff': 'utf-16le'
        };

        // WHATWG "ASCII lowercase": only A-Z, unlike String.prototype.toLowerCase(),
        // which Unicode-case-folds characters like U+212A KELVIN SIGN to 'k' --
        // that would make the label "Koi8-r" wrongly match "koi8-r".
        function asciiLowercaseLabel(text) {
          return text.replace(/[A-Z]/g, (letter) => letter.toLowerCase());
        }

        // WHATWG "get an encoding": trim ASCII whitespace, ASCII-lowercase,
        // then match against the label table. Lexbor owns the actual table;
        // this just normalizes the same way the spec's lookup does.
        function normalizeEncodingLabel(label) {
          const trimmed = asciiLowercaseLabel(String(label == null ? 'utf-8' : label).replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, ''));
          return LEGACY_ENCODING_LABEL_ALIASES[trimmed] || trimmed;
        }

        function bomBytesFor(encoding) {
          if (encoding === 'utf-8') return [0xef, 0xbb, 0xbf];
          if (encoding === 'utf-16le') return [0xff, 0xfe];
          if (encoding === 'utf-16be') return [0xfe, 0xff];
          return null;
        }

        // Whether `bytes` (so far) could still be a prefix of `bom`: 'full' once
        // all of `bom` is present and matches, 'partial' while more bytes could still
        // complete a match, 'no' as soon as any byte mismatches (so e.g. two
        // leading NUL bytes are never held back waiting for a UTF-8 BOM that
        // starts with 0xEF).
        function bomPrefixMatch(bytes, bom) {
          const checkLength = Math.min(bytes.length, bom.length);
          for (let i = 0; i < checkLength; i++) {
            if (bytes[i] !== bom[i]) return 'no';
          }
          return bytes.length >= bom.length ? 'full' : 'partial';
        }

        function concatBytes(a, b) {
          const out = new Uint8Array(a.length + b.length);
          out.set(a, 0);
          out.set(b, a.length);
          return out;
        }

        // A buffer can be detached (ArrayBuffer.prototype.transfer()) as a side
        // effect of converting a later argument -- e.g. TextDecoder.decode(buf,
        // { get stream() { buf.buffer.transfer(0); return false; } }). Per spec
        // this is not an error: the detached input is simply treated as empty.
        function toBytes(input) {
          if (input instanceof Uint8Array) return input.buffer.detached ? new Uint8Array() : input;
          if (ArrayBuffer.isView(input)) {
            return input.buffer.detached ? new Uint8Array() : new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
          }
          if (input instanceof ArrayBuffer) return input.detached ? new Uint8Array() : new Uint8Array(input);
          return new Uint8Array();
        }

        class TextDecoder {
          constructor(label = 'utf-8', options = {}) {
            const normalized = normalizeEncodingLabel(label);
            const fatal = Boolean(options && options.fatal);
            if (typeof host.textDecoderCreate === 'function') {
              const created = host.textDecoderCreate(normalized, fatal);
              if (!created) {
                throw new RangeError(`Failed to construct 'TextDecoder': The encoding label provided ('${label}') is invalid.`);
              }
              this._handle = created.handle;
              // Lexbor's encoding_data->name is a display-style label (e.g.
              // "GBK", "Big5"); the spec requires the ASCII-lowercased form.
              this.encoding = created.encoding.toLowerCase();
            } else {
              // No native encoding binding (e.g. isolated JS-shim unit tests
              // with no C++ host bridge): fall back to pure-JS UTF-8 only,
              // matching this class's pre-native-binding behavior.
              if (normalized !== 'utf-8' && normalized !== 'utf8') {
                throw new RangeError(`Failed to construct 'TextDecoder': The encoding label provided ('${label}') is invalid.`);
              }
              this._handle = null;
              this.encoding = 'utf-8';
            }
            this.fatal = fatal;
            this.ignoreBOM = Boolean(options && options.ignoreBOM);
            // True while a `{stream: true}` sequence is in progress; false
            // (including right before this decoder has done anything) means
            // the *next* decode() call starts a fresh sequence, eligible for
            // BOM stripping again from a clean slate.
            this._streaming = false;
            this._resetBomState();
          }

          // `_bomPending` is null once resolved for the rest of this
          // sequence, or when there is no BOM concept for this encoding /
          // ignoreBOM is set; otherwise it holds bytes not yet fed to the
          // decoder because they could still turn out to be a BOM.
          _resetBomState() {
            this._bomPending = (!this.ignoreBOM && bomBytesFor(this.encoding)) ? new Uint8Array(0) : null;
          }

          decode(input = new Uint8Array(), options = {}) {
            // Read `options.stream` before converting `input` to bytes: a
            // getter on `options` can detach `input`'s buffer as a side
            // effect (see toBytes()), and per spec that ordering is what
            // makes the detached-input case observable as empty, not an
            // exception from reading stale byte data.
            const streaming = Boolean(options && options.stream);
            let bytes = toBytes(input);

            if (!this._streaming) {
              // Starting a fresh sequence: this covers both the first-ever
              // call and a call following one that finalized or threw (a
              // fatal error always ends the sequence, per spec, regardless
              // of what `stream` that failed call itself requested).
              this._resetBomState();
            }

            if (this._bomPending) {
              const bom = bomBytesFor(this.encoding);
              const combined = concatBytes(this._bomPending, bytes);
              const match = bomPrefixMatch(combined, bom);
              if (match === 'partial' && streaming) {
                // Could still be a BOM or could still be ordinary data (e.g. a
                // lone leading 0xFF byte of a UTF-16LE code unit); neither is
                // decided until more bytes arrive or the stream ends.
                this._bomPending = combined;
                this._streaming = true;
                return '';
              }
              bytes = match === 'full' ? combined.subarray(bom.length) : combined;
              this._bomPending = null;
            }

            const result = this._handle === null
              ? utf8DecodeReplacement(bytes, { fatal: this.fatal })
              : host.textDecoderDecode(this._handle, bytes, !streaming);
            if (result === null || result === undefined) {
              this._streaming = false;
              throw new TypeError('The encoded data was not valid.');
            }
            this._streaming = streaming;
            return result;
          }
        }

        class Headers {
          constructor(init = undefined) {
            this._map = Object.create(null);
            if (init == null) return;
            if (init instanceof Headers) {
              init.forEach((value, name) => this.append(name, value));
            } else if (typeof init[Symbol.iterator] === 'function') {
              for (const pair of init) this.append(pair[0], pair[1]);
            } else {
              for (const name of Object.keys(init)) this.append(name, init[name]);
            }
          }

          _name(name) {
            const text = String(name).toLowerCase();
            if (!/^[!#$%&'*+\-.^_`|~0-9a-z]+$/.test(text)) throw new TypeError('Invalid header name');
            return text;
          }
          _value(value) {
            const text = String(value).trim();
            if (/[\0\r\n]/.test(text)) throw new TypeError('Invalid header value');
            return text;
          }
          append(name, value) {
            name = this._name(name);
            value = this._value(value);
            this._map[name] = this._map[name] == null ? value : `${this._map[name]}, ${value}`;
          }
          set(name, value) { this._map[this._name(name)] = this._value(value); }
          get(name) { return this._map[this._name(name)] ?? null; }
          has(name) { return this._map[this._name(name)] != null; }
          delete(name) { delete this._map[this._name(name)]; }
          forEach(callback, thisArg = undefined) {
            for (const name of Object.keys(this._map).sort()) callback.call(thisArg, this._map[name], name, this);
          }
          entries() { return Object.keys(this._map).sort().map((name) => [name, this._map[name]])[Symbol.iterator](); }
          keys() { return Object.keys(this._map).sort()[Symbol.iterator](); }
          values() { return Object.keys(this._map).sort().map((name) => this._map[name])[Symbol.iterator](); }
          [Symbol.iterator]() { return this.entries(); }
        }

        function encodeUtf8(text) {
          const out = [];
          for (let i = 0; i < text.length; ++i) {
            const code = text.charCodeAt(i);
            if (code < 0x80) out.push(code);
            else if (code < 0x800) out.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
            else out.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
          }
          return new Uint8Array(out);
        }

        function decodeUtf8(bytes) {
          let out = '';
          for (let i = 0; i < bytes.length;) {
            const b1 = bytes[i++];
            if (b1 < 0x80) {
              out += String.fromCharCode(b1);
            } else if (b1 < 0xe0) {
              const b2 = bytes[i++] & 0x3f;
              out += String.fromCharCode(((b1 & 0x1f) << 6) | b2);
            } else {
              const b2 = bytes[i++] & 0x3f;
              const b3 = bytes[i++] & 0x3f;
              out += String.fromCharCode(((b1 & 0x0f) << 12) | (b2 << 6) | b3);
            }
          }
          return out;
        }

        function blobPartBytes(part) {
          if (part == null) return new Uint8Array(0);
          if (part instanceof Uint8Array) return part;
          if (part instanceof ArrayBuffer) return new Uint8Array(part);
          if (ArrayBuffer.isView(part)) return new Uint8Array(part.buffer, part.byteOffset, part.byteLength);
          if (part instanceof Blob) return part._bytes;
          return encodeUtf8(String(part));
        }

        class Blob {
          constructor(parts = [], options = {}) {
            const chunks = [];
            let size = 0;
            for (const part of parts) {
              const bytes = blobPartBytes(part);
              chunks.push(bytes);
              size += bytes.length;
            }
            this._bytes = new Uint8Array(size);
            let offset = 0;
            for (const chunk of chunks) {
              this._bytes.set(chunk, offset);
              offset += chunk.length;
            }
            this.size = size;
            this.type = options.type ? String(options.type).toLowerCase() : '';
          }

          slice(start = 0, end = this.size, type = '') {
            const from = start < 0 ? Math.max(this.size + start, 0) : Math.min(start, this.size);
            const to = end < 0 ? Math.max(this.size + end, 0) : Math.min(end, this.size);
            const blob = new Blob([], { type });
            blob._bytes = this._bytes.slice(Math.min(from, to), Math.max(from, to));
            blob.size = blob._bytes.length;
            return blob;
          }
          arrayBuffer() {
            const buffer = new ArrayBuffer(this._bytes.length);
            new Uint8Array(buffer).set(this._bytes);
            return Promise.resolve(buffer);
          }
          text() {
            return Promise.resolve(decodeUtf8(this._bytes));
          }
        }

        class File extends Blob {
          constructor(parts, name, options = {}) {
            super(parts, options);
            this.name = String(name);
            this.lastModified = options.lastModified === undefined ? Date.now() : Number(options.lastModified);
          }
        }

        class Request {
          constructor(input, init = {}) {
            const source = input instanceof Request ? input : null;
            this.url = source ? source.url : new URL(String(input), global.location.href || undefined).href;
            this.method = String(init.method || (source && source.method) || 'GET').toUpperCase();
            this.headers = new Headers(init.headers || (source && source.headers) || undefined);
            this.body = init.body !== undefined ? init.body : source ? source.body : null;
            this.mode = init.mode || (source && source.mode) || 'cors';
            this.credentials = normalizeCredentials(init.credentials || (source && source.credentials) || 'same-origin');
            this.cache = init.cache || (source && source.cache) || 'default';
            this.redirect = init.redirect || (source && source.redirect) || 'follow';
            this.referrer = normalizeReferrer(
              Object.prototype.hasOwnProperty.call(init, 'referrer')
                ? init.referrer
                : (source ? source.referrer : 'about:client'));
            this.signal = init.signal || (source && source.signal) || null;
          }

          clone() { return new Request(this); }
        }

        class Response {
          constructor(body = null, init = {}) {
            // Raw body kept internally; `body` is a spec ReadableStream getter
            // (below) so fetch clients that stream the response — e.g. Angular's
            // Fetch backend calling response.body.getReader() — work.
            this._bodyInit = body;
            this.status = init.status === undefined ? 200 : Number(init.status);
            this.statusText = init.statusText === undefined ? '' : String(init.statusText);
            this.headers = new Headers(init.headers);
            this.url = init.url || '';
            this.redirected = Boolean(init.redirected);
            this.type = 'default';
            this.ok = this.status >= 200 && this.status < 300;
          }

          get body() {
            if (this._bodyInit == null) return null;
            if (this._bodyStream === undefined) {
              const raw = this._bodyInit instanceof Blob ? this._bodyInit._bytes : this._bodyInit;
              this._bodyStream = global.ReadableStream._fromBody(raw);
            }
            return this._bodyStream;
          }
          get bodyUsed() {
            return Boolean(this._bodyStream && this._bodyStream.locked);
          }

          text() {
            if (this._bodyInit instanceof Blob) return this._bodyInit.text();
            if (this._bodyInit == null) return Promise.resolve('');
            return Promise.resolve(String(this._bodyInit));
          }

          arrayBuffer() {
            if (this._bodyInit instanceof Blob) return this._bodyInit.arrayBuffer();
            return new Blob([this._bodyInit == null ? '' : String(this._bodyInit)]).arrayBuffer();
          }

          json() { return this.text().then((text) => JSON.parse(text)); }
          clone() {
            return new Response(this._bodyInit, {
              status: this.status,
              statusText: this.statusText,
              headers: this.headers,
              url: this.url,
              redirected: this.redirected
            });
          }

          static json(data, init = {}) {
            const headers = new Headers(init.headers);
            if (!headers.has('content-type')) headers.set('content-type', 'application/json');
            return new Response(JSON.stringify(data), { ...init, headers });
          }

          static error() {
            const response = new Response(null, { status: 0, statusText: '' });
            response.type = 'error';
            response.ok = false;
            return response;
          }

          static redirect(url, status = 302) {
            return new Response(null, { status, headers: { location: String(url) } });
          }
        }

        function responseHeadersFromHost(response) {
          const headers = new Headers();
          if (response && response.headers && typeof response.headers[Symbol.iterator] === 'function') {
            for (const pair of response.headers) {
              const name = String(pair[0] || '');
              if (name.toLowerCase() === 'set-cookie' || name.toLowerCase() === 'set-cookie2') continue;
              headers.append(name, pair[1]);
            }
          }
          if (response && response.mimeType && !headers.has('content-type')) headers.set('content-type', response.mimeType);
          return headers;
        }

        function normalizeCredentials(value) {
          const credentials = String(value || 'same-origin');
          if (credentials === 'omit' || credentials === 'same-origin' || credentials === 'include') return credentials;
          throw new TypeError('Invalid credentials mode');
        }

        function normalizeReferrer(value) {
          if (value === undefined || value === null || value === 'about:client') return 'about:client';
          const referrer = String(value);
          if (referrer === '' || referrer === 'no-referrer') return '';
          return new URL(referrer, global.location.href || undefined).href;
        }

        function generateMultipartBoundary() {
          let boundary = '----PageCoreFormBoundary';
          for (let i = 0; i < 24; i++) boundary += Math.floor(Math.random() * 36).toString(36);
          return boundary;
        }

        function escapeMultipartFieldName(value) {
          return String(value).replace(/\r/g, '%0D').replace(/\n/g, '%0A').replace(/"/g, '%22');
        }

        // Serialize a FormData body as multipart/form-data and, unless the caller
        // already set one, declare the matching Content-Type (with the generated
        // boundary) on the request headers. The C++ bridge carries the request
        // body as a single UTF-8 string, so binary file parts are transmitted as
        // their decoded text — text fields (the common case) round-trip exactly.
        function multipartFormDataBody(formData, headers) {
          const boundary = generateMultipartBoundary();
          if (headers && typeof headers.has === 'function' && !headers.has('content-type')) {
            headers.set('content-type', `multipart/form-data; boundary=${boundary}`);
          }
          let body = '';
          for (const [name, value] of formData) {
            body += `--${boundary}\r\n`;
            if (value instanceof Blob) {
              const filename = value instanceof File ? value.name : 'blob';
              body += `Content-Disposition: form-data; name="${escapeMultipartFieldName(name)}"; filename="${escapeMultipartFieldName(filename)}"\r\n`;
              body += `Content-Type: ${value.type || 'application/octet-stream'}\r\n\r\n`;
              body += `${decodeUtf8(value._bytes)}\r\n`;
            } else {
              body += `Content-Disposition: form-data; name="${escapeMultipartFieldName(name)}"\r\n\r\n`;
              body += `${value}\r\n`;
            }
          }
          body += `--${boundary}--\r\n`;
          return body;
        }

        function bodyText(body, headers = null) {
          if (body == null) return '';
          if (body instanceof Blob) return decodeUtf8(body._bytes);
          if (body instanceof Uint8Array) return decodeUtf8(body);
          if (body instanceof ArrayBuffer) return decodeUtf8(new Uint8Array(body));
          if (ArrayBuffer.isView(body)) return decodeUtf8(new Uint8Array(body.buffer, body.byteOffset, body.byteLength));
          // FormData lives in the forms module, which depends on web, so web cannot
          // import it; the forms module stashes the class on ctx for this check.
          if (ctx.FormData && body instanceof ctx.FormData) return multipartFormDataBody(body, headers);
          return String(body);
        }

        function headerPairs(headers) {
          return headers ? Array.from(headers.entries()) : [];
        }

        function loadHostResource(url, kind = 'other', init = {}) {
          if (!host || typeof host.loadResource !== 'function') {
            throw new Error('resource loading is not available');
          }
          return host.loadResource(
            absoluteURL(url),
            kind,
            init.method || 'GET',
            init.body == null ? '' : String(init.body),
            headerPairs(init.headers),
            normalizeCredentials(init.credentials || 'same-origin'),
            normalizeReferrer(Object.prototype.hasOwnProperty.call(init, 'referrer') ? init.referrer : 'about:client')
          );
        }

        // Headers-object-aware wrapper over the core async loader (which takes
        // raw header pairs). Returns a promise of the host result object.
        function loadHostResourceAsync(url, kind = 'other', init = {}) {
          return coreLoadHostResourceAsync(url, kind, {
            method: init.method || 'GET',
            body: init.body == null ? '' : String(init.body),
            headers: headerPairs(init.headers),
            credentials: normalizeCredentials(init.credentials || 'same-origin'),
            referrer: Object.prototype.hasOwnProperty.call(init, 'referrer') ? init.referrer : 'about:client',
            onStarted: init.onStarted
          });
        }

        class XMLHttpRequest extends EventTarget {
          constructor() {
            super();
            this.readyState = XMLHttpRequest.UNSENT;
            this.response = '';
            this.responseText = '';
            this.responseType = '';
            this.responseURL = '';
            this.status = 0;
            this.statusText = '';
            this.timeout = 0;
            this.withCredentials = false;
            this.upload = new EventTarget();
            this.onreadystatechange = null;
            this.onload = null;
            this.onerror = null;
            this.onabort = null;
            this.onloadend = null;
            this.ontimeout = null;
            this._method = 'GET';
            this._url = '';
            this._async = true;
            this._requestHeaders = new Headers();
            this._responseHeaders = new Headers();
            this._aborted = false;
            this._activityPending = false;
            this._loadId = null;
          }

          open(method, url, async = true) {
            this._method = String(method || 'GET').toUpperCase();
            this._url = absoluteURL(url);
            this._async = async !== false;
            this._aborted = false;
            this.readyState = XMLHttpRequest.OPENED;
            this._fire('readystatechange');
          }

          setRequestHeader(name, value) {
            if (this.readyState !== XMLHttpRequest.OPENED) throw new DOMException('XMLHttpRequest is not open.', 'InvalidStateError');
            this._requestHeaders.append(name, value);
          }

          getResponseHeader(name) {
            return this._responseHeaders.get(name);
          }

          getAllResponseHeaders() {
            const lines = [];
            this._responseHeaders.forEach((value, name) => lines.push(`${name}: ${value}`));
            return lines.length ? lines.join('\r\n') + '\r\n' : '';
          }

          overrideMimeType(mimeType) {
            this._overrideMimeType = String(mimeType);
          }

          abort() {
            this._aborted = true;
            if (this._loadId != null) {
              cancelResourceLoad(this._loadId);
              this._loadId = null;
            }
            this.status = 0;
            this.statusText = '';
            this.readyState = XMLHttpRequest.DONE;
            this._fire('readystatechange');
            this._fire('abort');
            this._fire('loadend');
            this._endActivity();
          }

          _applyLoaded(loaded) {
            this.status = loaded.status === undefined ? 200 : Number(loaded.status);
            this.statusText = loaded.statusText === undefined ? '' : String(loaded.statusText);
            this.responseURL = loaded.url || this._url;
            this._responseHeaders = responseHeadersFromHost(loaded);
            if (this._overrideMimeType) this._responseHeaders.set('content-type', this._overrideMimeType);
            this.responseText = loaded.body || '';
            if (this.responseType === 'json') {
              // A JSON responseType whose body is not valid JSON still loads
              // successfully; the response attribute simply yields null. Parse
              // outside the fatal path so it never turns a load into an error.
              try {
                this.response = this.responseText ? JSON.parse(this.responseText) : null;
              } catch (_jsonError) {
                this.response = null;
              }
            } else {
              this.response = this.responseText;
            }
          }

          _finishSuccess() {
            this.readyState = XMLHttpRequest.HEADERS_RECEIVED;
            this._fire('readystatechange');

            this.readyState = XMLHttpRequest.LOADING;
            this._fire('readystatechange');

            this.readyState = XMLHttpRequest.DONE;
            this._fire('readystatechange');
            this._fire('load');
            this._fire('loadend');
            this._endActivity();
          }

          _finishError() {
            this.status = 0;
            this.statusText = '';
            this.response = '';
            this.responseText = '';
            this.readyState = XMLHttpRequest.DONE;
            this._fire('readystatechange');
            this._fire('error');
            this._fire('loadend');
            this._endActivity();
          }

          send(body = null) {
            if (this.readyState !== XMLHttpRequest.OPENED) throw new DOMException('XMLHttpRequest is not open.', 'InvalidStateError');
            this._beginActivity();
            const init = {
              method: this._method,
              body: bodyText(body, this._requestHeaders),
              headers: this._requestHeaders,
              credentials: this.withCredentials ? 'include' : 'same-origin',
              referrer: 'about:client'
            };

            if (!this._async) {
              // Synchronous XHR stays on the blocking host loader.
              if (this._aborted) {
                this._endActivity();
                return;
              }
              let loaded = null;
              try {
                loaded = loadHostResource(this._url, 'other', init);
              } catch (_error) {
                if (this._aborted) {
                  this._endActivity();
                  return;
                }
                this._finishError();
                return;
              }
              if (this._aborted) {
                this._endActivity();
                return;
              }
              this._applyLoaded(loaded);
              this._finishSuccess();
              return;
            }

            // Async XHR: the transfer starts immediately; the readystatechange
            // sequence runs when the completion task delivers the result.
            loadHostResourceAsync(this._url, 'other', {
              ...init,
              onStarted: (id) => { this._loadId = id; }
            }).then(
              (loaded) => {
                this._loadId = null;
                if (this._aborted) {
                  this._endActivity();
                  return;
                }
                this._applyLoaded(loaded);
                this._finishSuccess();
              },
              (_error) => {
                this._loadId = null;
                if (this._aborted) {
                  this._endActivity();
                  return;
                }
                this._finishError();
              });
          }

          _beginActivity() {
            if (this._activityPending) return;
            this._activityPending = true;
            activityBegin('xhr-fetch');
          }

          _endActivity() {
            if (!this._activityPending) return;
            this._activityPending = false;
            activityEnd('xhr-fetch');
          }

          _fire(type) {
            this.dispatchEvent(new Event(type));
          }
        }

        for (const [name, value] of Object.entries({
          UNSENT: 0,
          OPENED: 1,
          HEADERS_RECEIVED: 2,
          LOADING: 3,
          DONE: 4
        })) {
          defineValue(XMLHttpRequest, name, value, true);
          defineValue(XMLHttpRequest.prototype, name, value, true);
        }

        class Storage {
          constructor() {
            this._store = Object.create(null);
          }

          get length() { return Object.keys(this._store).length; }
          key(index) { return Object.keys(this._store)[Number(index)] || null; }
          getItem(key) {
            key = String(key);
            return Object.prototype.hasOwnProperty.call(this._store, key) ? this._store[key] : null;
          }
          setItem(key, value) { this._store[String(key)] = String(value); }
          removeItem(key) { delete this._store[String(key)]; }
          clear() { this._store = Object.create(null); }
        }

        class XMLSerializer {
          serializeToString(node) {
            if (!node) return '';
            if (node.nodeType === 9) return node.documentElement ? node.documentElement.outerHTML : '';
            if (node.nodeType === 11) return [...node.childNodes].map((child) => this.serializeToString(child)).join('');
            if (node.nodeType === 3) return node.data;
            return node.outerHTML || node.innerHTML || '';
          }
        }

        class DOMParser {
          parseFromString(html) {
            const parsed = document.implementation && document.implementation.createHTMLDocument
              ? document.implementation.createHTMLDocument('')
              : document;
            if (parsed.body) parsed.body.innerHTML = String(html ?? '');
            return parsed;
          }
        }

        // Macrotasks live in the host's libuv-backed event loop; this registry
        // only maps task ids to their JS callbacks. Only plain integer ids
        // cross the C++ boundary in either direction.
        const taskRegistry = new Map();
        let nextTaskId = 1;
        let timerNestingLevel = 0;

        function normalizeDelay(delay) {
          const value = Number(delay);
          if (!Number.isFinite(value) || value < 0) return 0;
          return value;
        }

        function reportTaskError(error) {
          try {
            if (global.console && typeof global.console.error === 'function') {
              global.console.error(error);
            } else if (host && typeof host.log === 'function') {
              host.log('error', formatErrorForLog(error));
            }
          } catch (_reportError) {
          }
        }

        function callTaskCallback(task) {
          if (typeof task.callback === 'function') {
            try {
              task.callback.call(global, ...task.args);
            } catch (error) {
              reportTaskError(error);
            }
          }
        }

        function locationFromURL(href) {
          let url = null;
          try {
            url = href ? new URL(href) : null;
          } catch (_error) {
            url = null;
          }
          const location = {
            href: url ? url.href : String(href || ''),
            protocol: url ? url.protocol : '',
            host: url ? url.host : '',
            hostname: url ? url.hostname : '',
            port: url ? url.port : '',
            pathname: url ? url.pathname : '',
            search: url ? url.search : '',
            hash: url ? url.hash : '',
            origin: url ? url.origin : '',
            assign(value) {
              const next = new URL(value, this.href || undefined);
              Object.assign(this, locationFromURL(next.href));
            },
            replace(value) { this.assign(value); },
            reload() {},
            toString() { return this.href; }
          };
          return location;
        }

        function cssLengthToPx(value) {
          const text = String(value || '').trim().toLowerCase();
          const match = /^(-?(?:\d+|\d*\.\d+))(px|em|rem)?$/.exec(text);
          if (!match) return NaN;
          const number = Number(match[1]);
          if (!Number.isFinite(number)) return NaN;
          return match[2] === 'em' || match[2] === 'rem' ? number * 16 : number;
        }

        function viewportValueForFeature(feature) {
          try {
            if (feature === 'width') return Number(global.innerWidth);
            if (feature === 'height') return Number(global.innerHeight);
          } catch (_viewportError) {
          }
          return NaN;
        }

        function evaluateMediaFeature(feature, value) {
          const normalized = String(feature || '').trim().toLowerCase();
          const lengthMatch = /^(min-|max-)?(width|height)$/.exec(normalized);
          if (lengthMatch) {
            const viewportValue = viewportValueForFeature(lengthMatch[2]);
            const expected = cssLengthToPx(value);
            if (!Number.isFinite(viewportValue) || !Number.isFinite(expected)) return false;
            if (lengthMatch[1] === 'min-') return viewportValue >= expected;
            if (lengthMatch[1] === 'max-') return viewportValue <= expected;
            return viewportValue === expected;
          }

          const ratioMatch = /^(min-|max-)?aspect-ratio$/.exec(normalized);
          if (ratioMatch) {
            const ratio = /^\s*(\d+(?:\.\d+)?)\s*\/\s*(\d+(?:\.\d+)?)\s*$/.exec(String(value || ''));
            if (!ratio) return false;
            const width = viewportValueForFeature('width');
            const height = viewportValueForFeature('height');
            const expected = Number(ratio[1]) / Number(ratio[2]);
            const actual = height > 0 ? width / height : NaN;
            if (!Number.isFinite(actual) || !Number.isFinite(expected)) return false;
            if (ratioMatch[1] === 'min-') return actual >= expected;
            if (ratioMatch[1] === 'max-') return actual <= expected;
            return actual === expected;
          }

          return false;
        }

        function mediaQueryParts(query) {
          return String(query).split(',').map((part) => part.trim()).filter((part) => part.length > 0);
        }

        function isRecognizedMediaQueryItem(query) {
          let rest = String(query || '').trim().toLowerCase();
          if (!rest) return true;
          if (rest.startsWith('not ')) {
            rest = rest.slice(4).trim();
          } else if (rest.startsWith('only ')) {
            rest = rest.slice(5).trim();
          }

          const parts = rest.split(/\s+and\s+/).map((part) => part.trim()).filter(Boolean);
          if (parts.length === 0) return false;
          let mediaTypeCount = 0;
          for (let index = 0; index < parts.length; index++) {
            const part = parts[index];
            if (part === 'all' || part === 'screen' || part === 'print') {
              if (index !== 0) return false;
              mediaTypeCount++;
              continue;
            }
            if (/^\(\s*[a-z-]+(?:\s*:\s*[^)]+)?\s*\)$/.test(part)) continue;
            return false;
          }
          return mediaTypeCount <= 1;
        }

        function serializeMediaQuery(query) {
          const original = String(query ?? '');
          const trimmed = original.trim();
          if (trimmed === '') return '';
          const parts = mediaQueryParts(trimmed);
          if (parts.length === 0) return '';

          const serialized = [];
          for (const part of parts) {
            if (!isRecognizedMediaQueryItem(part)) return 'not all';
            serialized.push(part
              .replace(/\s+/g, ' ')
              .replace(/^all\s+and\s+/i, '')
              .replace(/^only\s+all\s+and\s+/i, ''));
          }
          return serialized.join(', ');
        }

        function evaluateMediaQueryItem(query) {
          const text = String(query || '').trim().toLowerCase();
          if (!text) return true;

          let rest = text;
          let negated = false;
          if (rest.startsWith('not ')) {
            negated = true;
            rest = rest.slice(4).trim();
          } else if (rest.startsWith('only ')) {
            rest = rest.slice(5).trim();
          }

          const parts = rest.split(/\s+and\s+/).map((part) => part.trim()).filter(Boolean);
          if (parts.length === 0) return false;

          let mediaTypeMatched = false;
          for (const part of parts) {
            if (part === 'all' || part === 'screen') {
              mediaTypeMatched = true;
              continue;
            }
            if (part === 'print') return negated;

            const feature = /^\(\s*([a-z-]+)\s*:\s*([^)]+)\s*\)$/.exec(part);
            if (!feature || !evaluateMediaFeature(feature[1], feature[2])) {
              return negated;
            }
          }

          const matched = mediaTypeMatched || parts.some((part) => part.startsWith('('));
          return negated ? !matched : matched;
        }

        function evaluateSimpleMediaQuery(query) {
          const serialized = serializeMediaQuery(query);
          if (serialized === 'not all') return false;
          const parts = mediaQueryParts(serialized);
          if (parts.length === 0) return true;
          return parts.some((part) => evaluateMediaQueryItem(part));
        }

        class MediaQueryListEvent extends Event {
          constructor(type, init = {}) {
            const normalized = init && typeof init === 'object' ? init : {};
            super(type, normalized);
            this.media = normalized.media === undefined ? '' : String(normalized.media);
            this.matches = Boolean(normalized.matches);
          }

          get [Symbol.toStringTag]() { return 'MediaQueryListEvent'; }
        }

        class MediaQueryList extends EventTarget {
          constructor(query) {
            super();
            this._media = serializeMediaQuery(query);
            this.onchange = null;
          }

          get matches() { return evaluateSimpleMediaQuery(this._media); }
          get media() { return this._media; }
          addListener(callback) { this.addEventListener('change', callback); }
          removeListener(callback) { this.removeEventListener('change', callback); }
          get [Symbol.toStringTag]() { return 'MediaQueryList'; }
        }

        function makeMediaQueryList(query) {
          return new MediaQueryList(query);
        }

        // Deliberately a no-op: PageCore has no browser observation loop
        // (see docs/browser-api-support.md), so there is no layout-change
        // signal to drive callbacks from. This exists purely so a page script
        // that references `ResizeObserver` unconditionally (rather than
        // feature-detecting it) doesn't throw a ReferenceError and abort.
        // observe()/unobserve()/disconnect() track targets for API-shape
        // fidelity only; the callback is never invoked.
        class ResizeObserver {
          constructor(callback) {
            if (typeof callback !== 'function') {
              throw new TypeError('ResizeObserver requires a callback function');
            }
            this._callback = callback;
            this._targets = new Set();
          }

          observe(target, _options) {
            if (!(target instanceof Element)) {
              throw new TypeError("Failed to execute 'observe' on 'ResizeObserver': parameter 1 is not of type 'Element'");
            }
            this._targets.add(target);
          }

          unobserve(target) {
            this._targets.delete(target);
          }

          disconnect() {
            this._targets.clear();
          }

          get [Symbol.toStringTag]() { return 'ResizeObserver'; }
        }

        // Same rationale and shape as ResizeObserver above: a spec-shaped
        // no-op so page scripts that reference `IntersectionObserver`
        // unconditionally don't throw a ReferenceError. `root`/`rootMargin`/
        // `thresholds` are normalized and exposed read-only per spec, but
        // nothing ever calls the callback or produces entries.
        class IntersectionObserver {
          constructor(callback, options = {}) {
            if (typeof callback !== 'function') {
              throw new TypeError('IntersectionObserver requires a callback function');
            }
            const opts = options && typeof options === 'object' ? options : {};
            this._callback = callback;
            this._targets = new Set();
            this._root = opts.root === undefined ? null : opts.root;
            this._rootMargin = opts.rootMargin === undefined ? '0px 0px 0px 0px' : String(opts.rootMargin);
            const rawThresholds = opts.threshold === undefined ? [0] : opts.threshold;
            const thresholds = (Array.isArray(rawThresholds) ? rawThresholds : [rawThresholds]).map(Number);
            this._thresholds = (thresholds.length ? thresholds : [0]).slice().sort((a, b) => a - b);
          }

          get root() { return this._root; }
          get rootMargin() { return this._rootMargin; }
          get thresholds() { return this._thresholds.slice(); }

          observe(target) {
            if (!(target instanceof Element)) {
              throw new TypeError("Failed to execute 'observe' on 'IntersectionObserver': parameter 1 is not of type 'Element'");
            }
            this._targets.add(target);
          }

          unobserve(target) {
            this._targets.delete(target);
          }

          disconnect() {
            this._targets.clear();
          }

          takeRecords() { return []; }

          get [Symbol.toStringTag]() { return 'IntersectionObserver'; }
        }

        function getRandomValues(array) {
          if (!ArrayBuffer.isView(array) || array instanceof DataView) {
            throw new TypeError('Expected an integer typed array');
          }
          if (array.byteLength > 65536) {
            throw new DOMException('Quota exceeded.', 'QuotaExceededError');
          }
          if (!host || typeof host.randomBytes !== 'function') {
            throw new DOMException('A secure random source is not available', 'NotSupportedError');
          }
          const random = host.randomBytes(array.byteLength);
          const bytes = new Uint8Array(array.buffer, array.byteOffset, array.byteLength);
          for (let index = 0; index < bytes.length; index++) {
            bytes[index] = random[index] & 0xff;
          }
          return array;
        }

        function randomUUID() {
          const bytes = getRandomValues(new Uint8Array(16));
          bytes[6] = (bytes[6] & 0x0f) | 0x40;
          bytes[8] = (bytes[8] & 0x3f) | 0x80;
          const hex = Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0'));
          return `${hex.slice(0, 4).join('')}-${hex.slice(4, 6).join('')}-${hex.slice(6, 8).join('')}-${hex.slice(8, 10).join('')}-${hex.slice(10, 16).join('')}`;
        }

        function installWptHook() {
          if (global.__pagecore_wpt_installed) return;
          global.__pagecore_wpt_installed = true;
          global.__pagecore_wpt_done = false;
          global.__pagecore_wpt_failures = 0;

          function squash(value) {
            return String(value).replace(/[\u0000-\u001f\u007f-\u009f]+/g, ' ').replace(/\s+/g, ' ').trim();
          }

          global.__pagecore_wpt_oncomplete = function(tests, status) {
            if (global.__pagecore_wpt_done) return;
            const harnessNames = ['OK', 'ERROR', 'TIMEOUT', 'PRECONDITION_FAILED'];
            const subtestNames = ['PASS', 'FAIL', 'TIMEOUT', 'NOTRUN', 'PRECONDITION_FAILED'];
            const harness = harnessNames[status.status] || `UNKNOWN(${status.status})`;
            const counts = { PASS: 0, FAIL: 0, TIMEOUT: 0, NOTRUN: 0, PRECONDITION_FAILED: 0 };
            const lines = [`WPT HARNESS ${harness}${status.message ? ' | ' + squash(status.message) : ''}`];
            const subtests = [];
            for (const test of tests) {
              const state = subtestNames[test.status] || `UNKNOWN(${test.status})`;
              if (counts[state] !== undefined) counts[state]++;
              lines.push(`WPT ${state} ${squash(test.name)}${test.message ? ' | ' + squash(test.message) : ''}`);
              subtests.push({ name: test.name, status: state, message: test.message === undefined ? null : test.message });
            }
            lines.push(`WPT SUMMARY total=${tests.length} pass=${counts.PASS} fail=${counts.FAIL} timeout=${counts.TIMEOUT} notrun=${counts.NOTRUN} precondition_failed=${counts.PRECONDITION_FAILED}`);
            global.__pagecore_wpt_report = lines.join('\n') + '\n';
            global.__pagecore_wpt_json = JSON.stringify({ harness, message: status.message || null, subtests });
            global.__pagecore_wpt_failures = counts.FAIL + counts.TIMEOUT + counts.NOTRUN + (harness === 'OK' || harness === 'PRECONDITION_FAILED' ? 0 : 1);
            global.__pagecore_wpt_done = true;
          };

          Object.defineProperty(global, 'add_completion_callback', {
            configurable: true,
            get() { return undefined; },
            set(callback) {
              delete global.add_completion_callback;
              global.add_completion_callback = callback;
              if (typeof callback === 'function') {
                global.queueMicrotask(() => callback(global.__pagecore_wpt_oncomplete));
              }
            }
          });

          // Real event-sequence driver backing `test_driver_internal` (WPT
          // vendor extension point). Ships correct *event dispatch* for the
          // in-scope corpus (plain click, plain send_keys, single-pointer-source
          // Actions()) -- not full WebDriver semantics: no HTML activation
          // behavior (checkbox toggle, form submit, link navigation), no
          // key/wheel/none action sources, no WebDriver special-key codepoints.
          // Uses bridge.exactElementGeometry() rather than element.getBoundingClientRect():
          // real WebDriver's Get Element Rect always reflects the current DOM, but
          // getBoundingClientRect() can fall back to a stale-or-null approximation
          // once geometry_bounded_mode trips from unrelated reads elsewhere on the
          // page (see Page::element_geometry), which would resolve a freshly added
          // Actions() target to (0,0) instead of its real position.
          function centerPointOf(element) {
            if (!element || typeof element.__id !== 'number') return { x: 0, y: 0 };
            const geometry = bridge.exactElementGeometry(element.__id);
            if (!geometry) return { x: 0, y: 0 };
            return {
              x: geometry.borderX + geometry.borderWidth / 2,
              y: geometry.borderY + geometry.borderHeight / 2
            };
          }

          function pointFromCoords(coords) {
            const x = coords && Number.isFinite(Number(coords.x)) ? Number(coords.x) : 0;
            const y = coords && Number.isFinite(Number(coords.y)) ? Number(coords.y) : 0;
            return { x, y };
          }

          function dispatchMouseLike(element, type, point, button, buttons) {
            if (!element || typeof element.dispatchEvent !== 'function') return;
            const isPointerType = type === 'pointerdown' || type === 'pointerup';
            const init = {
              bubbles: true,
              cancelable: true,
              composed: true,
              view: global,
              detail: type === 'click' ? 1 : 0,
              clientX: point.x,
              clientY: point.y,
              screenX: point.x,
              screenY: point.y,
              button,
              buttons
            };
            if (isPointerType) {
              init.pointerId = 1;
              init.pointerType = 'mouse';
              init.isPrimary = true;
              element.dispatchEvent(new PointerEvent(type, init));
            } else {
              element.dispatchEvent(new MouseEvent(type, init));
            }
          }

          function pointerDownAt(element, point, button) {
            // Only the primary (left) button is exercised by any in-scope
            // test; `buttons` always toggles bit 0 here rather than the real
            // per-button bitmask (4/2/8/16 for middle/right/back/forward)
            // since no corpus file asserts a secondary-button `buttons` value.
            dispatchMouseLike(element, 'pointerdown', point, button, 1);
            dispatchMouseLike(element, 'mousedown', point, button, 1);
          }

          function pointerUpAt(element, point, button) {
            dispatchMouseLike(element, 'pointerup', point, button, 0);
            dispatchMouseLike(element, 'mouseup', point, button, 0);
          }

          function fireClick(element, point, button) {
            dispatchMouseLike(element, 'click', point, button, 0);
          }

          function syntheticClick(element, coords) {
            if (!element) throw new Error('test_driver_internal.click: element is required');
            const point = coords ? pointFromCoords(coords) : centerPointOf(element);
            pointerDownAt(element, point, 0);
            if (typeof element.focus === 'function') element.focus();
            pointerUpAt(element, point, 0);
            fireClick(element, point, 0);
          }

          // ASCII-only best-effort `KeyboardEvent.code`; the one in-scope
          // send_keys corpus file only sends plain printable characters, so a
          // WebDriver special-key-codepoint table is out of scope.
          function codeForChar(char) {
            if (char === ' ') return 'Space';
            if (/[a-z]/.test(char)) return 'Key' + char.toUpperCase();
            if (/[A-Z]/.test(char)) return 'Key' + char;
            if (/[0-9]/.test(char)) return 'Digit' + char;
            return '';
          }

          function syntheticSendKeys(element, keys) {
            if (!element) return;
            if (typeof element.focus === 'function') element.focus();
            for (const key of String(keys)) {
              const printable = key.length === 1 && key >= ' ' && key !== '';
              const base = {
                bubbles: true,
                cancelable: true,
                composed: true,
                view: global,
                key,
                code: codeForChar(key)
              };
              element.dispatchEvent(new KeyboardEvent('keydown', base));
              if (printable) {
                element.dispatchEvent(new KeyboardEvent('keypress', base));
                if ('value' in element) {
                  element.value = String(element.value || '') + key;
                  element.dispatchEvent(new Event('input', { bubbles: true, composed: true }));
                }
              }
              element.dispatchEvent(new KeyboardEvent('keyup', base));
            }
          }

          // Resolves a WebDriver Actions `pointerMove` tick's (x, y, origin) to a
          // document-space point. `origin` is the real in-process Element object
          // (no wire serialization, unlike real WebDriver) -- resolved through
          // centerPointOf(), i.e. the same in-view-center point test_driver.js
          // computes for click(), but read from exact geometry rather than
          // getBoundingClientRect(). "viewport"/"pointer" origins fall back to
          // raw x, y: no scroll model and no real multi-tick pointer tracking
          // make a truer "pointer" origin meaningless here, and no in-scope file
          // uses either.
          function resolvePointerMovePoint(action) {
            const dx = Number(action.x) || 0;
            const dy = Number(action.y) || 0;
            const origin = action.origin;
            if (origin instanceof Element) {
              const center = centerPointOf(origin);
              return { x: center.x + dx, y: center.y + dy };
            }
            return { x: dx, y: dy };
          }

          function elementAtPoint(point) {
            if (typeof document.elementFromPoint !== 'function') return null;
            return document.elementFromPoint(point.x, point.y);
          }

          // Minimal interpreter: only the `pointer` action source is
          // implemented, one tick per primitive. No `key`/`wheel`/`none`
          // sources, no touch/multi-touch, no true multi-source tick
          // synchronization -- no in-scope corpus file needs any of that.
          // Unsupported sources are skipped (not thrown), so a test degrades to
          // a clear assertion failure instead of a crashed harness.
          function syntheticActionSequence(actionsByInput) {
            let point = { x: 0, y: 0 };
            let downTarget = null;
            for (const source of actionsByInput || []) {
              if (!source || source.type !== 'pointer') {
                if (source && source.type && global.console && typeof global.console.warn === 'function') {
                  global.console.warn(`test_driver.action_sequence: "${source.type}" action source is not implemented by this vendor shim`);
                }
                continue;
              }
              for (const action of source.actions || []) {
                if (!action || action.type === 'pause') continue;
                if (action.type === 'pointerMove') {
                  point = resolvePointerMovePoint(action);
                } else if (action.type === 'pointerDown') {
                  const button = action.button === undefined ? 0 : Number(action.button);
                  downTarget = elementAtPoint(point);
                  if (downTarget) pointerDownAt(downTarget, point, button);
                } else if (action.type === 'pointerUp') {
                  const button = action.button === undefined ? 0 : Number(action.button);
                  const upTarget = elementAtPoint(point);
                  if (upTarget) {
                    pointerUpAt(upTarget, point, button);
                    if (downTarget && upTarget === downTarget) fireClick(upTarget, point, button);
                  }
                  downTarget = null;
                }
              }
            }
          }

          const driver = {
            click(element, coords) {
              return Promise.resolve().then(() => syntheticClick(element, coords));
            },
            send_keys(element, keys) {
              return Promise.resolve().then(() => syntheticSendKeys(element, keys));
            },
            action_sequence(actionsByInput) {
              return Promise.resolve().then(() => syntheticActionSequence(actionsByInput));
            }
          };

          let testDriverInternal = null;
          Object.defineProperty(global, 'test_driver_internal', {
            configurable: true,
            get() { return testDriverInternal; },
            set(value) {
              testDriverInternal = value || {};
              Object.assign(testDriverInternal, driver);
            }
          });
        }

      function isReadinessRelevantKind(kind) {
        return kind === 'timer'
          || kind === 'xhr-fetch'
          || kind === 'dynamic-script'
          || kind === 'dom-resource';
      }

      function queueTask(callback, delay = 0, args = [], kind = 'other', interval = false) {
        const id = nextTaskId++;
        let timeout = normalizeDelay(delay);
        const kindText = String(kind || 'other');
        const nesting = timerNestingLevel + 1;
        // HTML spec: timers nested more than 5 levels deep clamp to >= 4ms.
        if (nesting > 5 && timeout < 4) timeout = 4;
        taskRegistry.set(id, {
          callback,
          args: Array.isArray(args) ? args : [],
          interval: Boolean(interval),
          kind: kindText,
          nesting
        });
        const relevant = isReadinessRelevantKind(kindText) && !interval;
        if (!interval && timeout === 0 && host && typeof host.queueTask === 'function') {
          // Zero-delay one-shot tasks enqueue immediately (global FIFO across
          // sources) instead of round-tripping through a 0ms uv timer.
          host.queueTask(id, kindText);
        } else if (host && typeof host.scheduleTimer === 'function') {
          host.scheduleTimer(id, timeout, Boolean(interval), relevant);
        }
        // Without a host scheduler (the node unit-test harness) the task stays
        // registered; tests drive it explicitly through runTask(id).
        return id;
      }

      function queuePageTask(callback, kind = 'other', delay = 0) {
        return queueTask(callback, delay, [], kind, false);
      }

      function setTimeoutShim(callback, delay = 0, ...args) {
        return queueTask(callback, delay, args, 'timer', false);
      }

      function clearTask(id) {
        const existed = taskRegistry.delete(id);
        if (host && typeof host.cancelTimer === 'function') host.cancelTimer(Number(id) || 0);
        return existed;
      }

      function setIntervalShim(callback, delay = 0, ...args) {
        return queueTask(callback, delay, args, 'timer', true);
      }

      // Real animation frames: ids accumulate host-side until the ~16ms frame
      // timer fires; the C++ rendering phase then dispatches the whole batch
      // through runAnimationFrames below.
      const rafRegistry = new Map();

      function requestAnimationFrameShim(callback) {
        if (host && typeof host.requestAnimationFrame === 'function') {
          const id = nextTaskId++;
          rafRegistry.set(id, callback);
          host.requestAnimationFrame(id);
          return id;
        }
        return setTimeoutShim(() => callback(performanceNow()), 16);
      }

      function cancelAnimationFrameShim(id) {
        if (rafRegistry.delete(Number(id))) {
          if (host && typeof host.cancelAnimationFrame === 'function') host.cancelAnimationFrame(Number(id));
          return true;
        }
        return clearTask(id);
      }

      // Entry point for the C++ rendering phase (__pagecore_run_raf_callbacks).
      // Ids cancelled after the frame fired simply miss the registry.
      function runAnimationFrames(ids, now) {
        const timestamp = Number(now);
        const list = Array.isArray(ids) ? ids : [];
        let ran = 0;
        for (const rawId of list) {
          const id = Number(rawId);
          const callback = rafRegistry.get(id);
          if (!callback) continue;
          rafRegistry.delete(id);
          try {
            callback.call(global, timestamp);
            ran++;
          } catch (error) {
            reportTaskError(error);
          }
        }
        return ran;
      }

      function requestIdleCallbackShim(callback) {
        return setTimeoutShim(() => callback({ didTimeout: false, timeRemaining: () => 0 }), 0);
      }

      function performanceNow() {
        return host && typeof host.now === 'function' ? Number(host.now()) : Date.now();
      }
      ctx.pagecoreNow = performanceNow;

      // Entry point the C++ event loop calls (as __pagecore_run_task) when a
      // queued task or fired timer is due. Ids cancelled between fire and run
      // simply miss the registry and are a no-op.
      function runTask(id) {
        const task = taskRegistry.get(Number(id));
        if (!task) return 0;
        if (!task.interval) taskRegistry.delete(Number(id));
        const previousNesting = timerNestingLevel;
        timerNestingLevel = task.nesting;
        try {
          callTaskCallback(task);
        } finally {
          timerNestingLevel = previousNesting;
        }
        return 1;
      }

      return {
        DOMRectReadOnly,
        DOMRect,
        DOMRectList,
        CaretPosition,
        Range,
        StaticRange,
        Selection,
        selection,
        URLSearchParams,
        URL,
        TextEncoder,
        TextDecoder,
        Headers,
        Blob,
        File,
        Request,
        Response,
        responseHeadersFromHost,
        bodyText,
        loadHostResource,
        loadHostResourceAsync,
        XMLHttpRequest,
        Storage,
        XMLSerializer,
        DOMParser,
        locationFromURL,
        MediaQueryList,
        MediaQueryListEvent,
        makeMediaQueryList,
        ResizeObserver,
        IntersectionObserver,
        getRandomValues,
        randomUUID,
        installWptHook,
        setTimeoutShim,
        clearTask,
        setIntervalShim,
        requestAnimationFrameShim,
        cancelAnimationFrameShim,
        runAnimationFrames,
        requestIdleCallbackShim,
        performanceNow,
        queuePageTask,
        runTask
      };
    }
  };
});
