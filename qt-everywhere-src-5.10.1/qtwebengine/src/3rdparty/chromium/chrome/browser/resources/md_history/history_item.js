// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @param {!Element} root
 * @param {?Element} boundary
 * @param {cr.ui.FocusRow.Delegate} delegate
 * @constructor
 * @extends {cr.ui.FocusRow}
 */
function HistoryFocusRow(root, boundary, delegate) {
  cr.ui.FocusRow.call(this, root, boundary, delegate);
  this.addItems();
}

HistoryFocusRow.prototype = {
  __proto__: cr.ui.FocusRow.prototype,

  /** @override */
  getCustomEquivalent: function(sampleElement) {
    var equivalent;

    if (this.getTypeForElement(sampleElement) == 'star')
      equivalent = this.getFirstFocusable('title');

    return equivalent ||
        cr.ui.FocusRow.prototype.getCustomEquivalent.call(this, sampleElement);
  },

  addItems: function() {
    this.destroy();

    assert(this.addItem('checkbox', '#checkbox'));
    assert(this.addItem('title', '#title'));
    assert(this.addItem('menu-button', '#menu-button'));

    this.addItem('star', '#bookmark-star');
  },
};

cr.define('md_history', function() {
  /**
   * @param {{lastFocused: Object}} historyItemElement
   * @constructor
   * @implements {cr.ui.FocusRow.Delegate}
   */
  function FocusRowDelegate(historyItemElement) {
    this.historyItemElement = historyItemElement;
  }

  FocusRowDelegate.prototype = {
    /**
     * @override
     * @param {!cr.ui.FocusRow} row
     * @param {!Event} e
     */
    onFocus: function(row, e) {
      this.historyItemElement.lastFocused = e.path[0];
    },

    /**
     * @override
     * @param {!cr.ui.FocusRow} row The row that detected a keydown.
     * @param {!Event} e
     * @return {boolean} Whether the event was handled.
     */
    onKeydown: function(row, e) {
      // Allow Home and End to move the history list.
      if (e.key == 'Home' || e.key == 'End')
        return true;

      // Prevent iron-list from changing the focus on enter.
      if (e.key == 'Enter')
        e.stopPropagation();

      return false;
    },
  };

  var HistoryItem = Polymer({
    is: 'history-item',

    properties: {
      // Underlying HistoryEntry data for this item. Contains read-only fields
      // from the history backend, as well as fields computed by history-list.
      item: {
        type: Object,
        observer: 'itemChanged_',
      },

      selected: {
        type: Boolean,
        reflectToAttribute: true,
      },

      isCardStart: {
        type: Boolean,
        reflectToAttribute: true,
      },

      isCardEnd: {
        type: Boolean,
        reflectToAttribute: true,
      },

      /** @type {Element} */
      lastFocused: {
        type: Object,
        notify: true,
      },

      ironListTabIndex: {
        type: Number,
        observer: 'ironListTabIndexChanged_',
      },

      selectionNotAllowed_: {
        type: Boolean,
        value: !loadTimeData.getBoolean('allowDeletingHistory'),
      },

      hasTimeGap: Boolean,

      index: Number,

      numberOfItems: Number,

      // Search term used to obtain this history-item.
      searchTerm: String,
    },

    /** @private {?HistoryFocusRow} */
    row_: null,

    /** @private {boolean} */
    mouseDown_: false,

    /** @override */
    attached: function() {
      Polymer.RenderStatus.afterNextRender(this, function() {
        this.row_ = new HistoryFocusRow(
            this.$['main-container'], null, new FocusRowDelegate(this));
        this.row_.makeActive(this.ironListTabIndex == 0);
        this.listen(this, 'focus', 'onFocus_');
        this.listen(this, 'dom-change', 'onDomChange_');
      });
    },

    /** @override */
    detached: function() {
      this.unlisten(this, 'focus', 'onFocus_');
      this.unlisten(this, 'dom-change', 'onDomChange_');
      if (this.row_)
        this.row_.destroy();
    },

    /**
     * @private
     */
    onFocus_: function() {
      // Don't change the focus while the mouse is down, as it prevents text
      // selection. Not changing focus here is acceptable because the checkbox
      // will be focused in onItemClick_() anyway.
      if (this.mouseDown_)
        return;

      if (this.lastFocused)
        this.row_.getEquivalentElement(this.lastFocused).focus();
      else
        this.row_.getFirstFocusable().focus();

      this.tabIndex = -1;
    },

    /**
     * @private
     */
    ironListTabIndexChanged_: function() {
      if (this.row_)
        this.row_.makeActive(this.ironListTabIndex == 0);
    },

    /**
     * @private
     */
    onDomChange_: function() {
      if (this.row_)
        this.row_.addItems();
    },

    /**
     * Toggle item selection whenever the checkbox or any non-interactive part
     * of the item is clicked.
     * @param {MouseEvent} e
     * @private
     */
    onItemClick_: function(e) {
      for (var i = 0; i < e.path.length; i++) {
        var elem = e.path[i];
        if (elem.id != 'checkbox' &&
            (elem.nodeName == 'A' || elem.nodeName == 'BUTTON')) {
          return;
        }
      }

      if (this.selectionNotAllowed_)
        return;

      this.$.checkbox.focus();
      this.fire('history-checkbox-select', {
        index: this.index,
        shiftKey: e.shiftKey,
      });
    },

    /**
     * @param {MouseEvent} e
     * @private
     */
    onItemMousedown_: function(e) {
      this.mouseDown_ = true;
      listenOnce(document, 'mouseup', function() {
        this.mouseDown_ = false;
      }.bind(this));
      // Prevent shift clicking a checkbox from selecting text.
      if (e.shiftKey)
        e.preventDefault();
    },

    /**
     * @private
     * @return {string}
     */
    getEntrySummary_: function() {
      var item = this.item;
      return loadTimeData.getStringF(
          'entrySummary', item.dateTimeOfDay,
          item.starred ? loadTimeData.getString('bookmarked') : '', item.title,
          item.domain);
    },

    /**
     * @param {boolean} selected
     * @return {string}
     * @private
     */
    getAriaChecked_: function(selected) {
      return selected ? 'true' : 'false';
    },

    /**
     * Remove bookmark of current item when bookmark-star is clicked.
     * @private
     */
    onRemoveBookmarkTap_: function() {
      if (!this.item.starred)
        return;

      if (this.$$('#bookmark-star') == this.root.activeElement)
        this.$['menu-button'].focus();

      var browserService = md_history.BrowserService.getInstance();
      browserService.removeBookmark(this.item.url);
      browserService.recordAction('BookmarkStarClicked');

      this.fire('remove-bookmark-stars', this.item.url);
    },

    /**
     * Fires a custom event when the menu button is clicked. Sends the details
     * of the history item and where the menu should appear.
     */
    onMenuButtonTap_: function(e) {
      this.fire('open-menu', {
        target: Polymer.dom(e).localTarget,
        index: this.index,
        item: this.item,
      });

      // Stops the 'tap' event from closing the menu when it opens.
      e.stopPropagation();
    },

    /**
     * Record metrics when a result is clicked. This is deliberately tied to
     * on-click rather than on-tap, as on-click triggers from middle clicks.
     */
    onLinkClick_: function() {
      var browserService = md_history.BrowserService.getInstance();
      browserService.recordAction('EntryLinkClick');

      if (this.searchTerm)
        browserService.recordAction('SearchResultClick');

      if (this.index == undefined)
        return;

      browserService.recordHistogram(
          'HistoryPage.ClickPosition',
          Math.min(this.index, UMA_MAX_BUCKET_VALUE), UMA_MAX_BUCKET_VALUE);

      if (this.index <= UMA_MAX_SUBSET_BUCKET_VALUE) {
        browserService.recordHistogram(
            'HistoryPage.ClickPositionSubset', this.index,
            UMA_MAX_SUBSET_BUCKET_VALUE);
      }
    },

    onLinkRightClick_: function() {
      md_history.BrowserService.getInstance().recordAction(
          'EntryLinkRightClick');
    },

    /**
     * Set the favicon image, based on the URL of the history item.
     * @private
     */
    itemChanged_: function() {
      this.$.icon.style.backgroundImage = cr.icon.getFavicon(this.item.url);
      this.listen(this.$['time-accessed'], 'mouseover', 'addTimeTitle_');
    },

    /**
     * @param {number} numberOfItems The number of items in the card.
     * @param {string} historyDate Date of the current result.
     * @param {string} search The search term associated with these results.
     * @return {string} The title for this history card.
     * @private
     */
    cardTitle_: function(numberOfItems, historyDate, search) {
      if (!search)
        return this.item.dateRelativeDay;
      return HistoryItem.searchResultsTitle(numberOfItems, search);
    },

    /** @private */
    addTimeTitle_: function() {
      var el = this.$['time-accessed'];
      el.setAttribute('title', new Date(this.item.time).toString());
      this.unlisten(el, 'mouseover', 'addTimeTitle_');
    },
  });

  /**
   * @param {number} numberOfResults
   * @param {string} searchTerm
   * @return {string} The title for a page of search results.
   */
  HistoryItem.searchResultsTitle = function(numberOfResults, searchTerm) {
    var resultId = numberOfResults == 1 ? 'searchResult' : 'searchResults';
    return loadTimeData.getStringF(
        'foundSearchResults', numberOfResults, loadTimeData.getString(resultId),
        searchTerm);
  };

  return {HistoryItem: HistoryItem};
});
