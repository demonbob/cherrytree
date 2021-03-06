/*
 * ct_treestore.cc
 *
 * Copyright 2017-2019 Giuseppe Penone <giuspen@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include <assert.h>
#include "ct_treestore.h"
#include "ct_doc_rw.h"
#include "ct_app.h"

CtAnchoredWidget::CtAnchoredWidget(const int& charOffset, const std::string& justification)
{
    _charOffset = charOffset;
    _justification = justification;
    _frame.set_shadow_type(Gtk::ShadowType::SHADOW_NONE);
    add(_frame);
}

void CtAnchoredWidget::insertInTextBuffer(Glib::RefPtr<Gsv::Buffer> rTextBuffer)
{
    _rTextChildAnchor = rTextBuffer->create_child_anchor(rTextBuffer->get_iter_at_offset(_charOffset));
    if (!_justification.empty())
    {
        Gtk::TextIter textIterStart = rTextBuffer->get_iter_at_child_anchor(_rTextChildAnchor);
        Gtk::TextIter textIterEnd = textIterStart;
        textIterEnd.forward_char();
        Glib::ustring tagName = CtMiscUtil::getTextTagNameExistOrCreate(CtConst::TAG_JUSTIFICATION, _justification);
        rTextBuffer->apply_tag_by_name(tagName, textIterStart, textIterEnd);
    }
}


CtTreeStore::CtTreeStore()
{
    _rTreeStore = Gtk::TreeStore::create(_columns);
}

CtTreeStore::~CtTreeStore()
{
    _iterDeleteAnchoredWidgets(getRootChildren());
    if (nullptr != _pCtSQLiteRead)
    {
        delete _pCtSQLiteRead;
    }
}

void CtTreeStore::_iterDeleteAnchoredWidgets(const Gtk::TreeModel::Children& children)
{
    for (Gtk::TreeIter treeIter = children.begin(); treeIter != children.end(); ++treeIter)
    {
        Gtk::TreeRow row = *treeIter;
        for (CtAnchoredWidget* pCtAnchoredWidget : row.get_value(_columns.colAnchoredWidgets))
        {
            delete pCtAnchoredWidget;
            //printf("~pCtAnchoredWidget\n");
        }
        row.get_value(_columns.colAnchoredWidgets).clear();

        _iterDeleteAnchoredWidgets(row.children());
    }
}

void CtTreeStore::setExpandedCollapsed(Gtk::TreeView* pTreeView,
                                       const Gtk::TreeModel::Children& children,
                                       const std::map<gint64,bool>& mapExpandedCollapsed)
{
    for (Gtk::TreeIter treeIter = children.begin(); treeIter != children.end(); ++treeIter)
    {
        Gtk::TreeRow row = *treeIter;
        gint64 nodeId = row.get_value(_columns.colNodeUniqueId);
        std::map<gint64,bool>::const_iterator it = mapExpandedCollapsed.find(nodeId);
        if (it != mapExpandedCollapsed.end() && it->second)
        {
            pTreeView->expand_row(_rTreeStore->get_path(treeIter), false/*open_all*/);
        }
        else if (CtApp::P_ctCfg->nodesBookmExp && _bookmarks.count(nodeId) > 0)
        {
            expandToTreeRow(pTreeView, row);
        }
        setExpandedCollapsed(pTreeView, row.children(), mapExpandedCollapsed);
    }
}

void CtTreeStore::expandToTreeRow(Gtk::TreeView* pTreeView, Gtk::TreeRow& row)
{
    Gtk::TreeIter iterParent = row.parent();
    if (static_cast<bool>(iterParent))
    {
        pTreeView->expand_to_path(_rTreeStore->get_path(iterParent));
    }
}

void CtTreeStore::treeviewSafeSetCursor(Gtk::TreeView* pTreeView, Gtk::TreeIter& treeIter)
{
    Gtk::TreeRow row = *treeIter;
    expandToTreeRow(pTreeView, row);
    pTreeView->set_cursor(_rTreeStore->get_path(treeIter));
}

void CtTreeStore::setTreePathTextCursorFromConfig(Gtk::TreeView* pTreeView, Gsv::View* pTextView)
{
    bool treeSelFromConfig{false};
    if (!CtApp::P_ctCfg->nodePath.empty())
    {
        Gtk::TreeIter treeIter = _rTreeStore->get_iter(CtApp::P_ctCfg->nodePath);
        if (static_cast<bool>(treeIter))
        {
            treeviewSafeSetCursor(pTreeView, treeIter);
            treeSelFromConfig = true;
            if (CtApp::P_ctCfg->treeClickExpand)
            {
                pTreeView->expand_row(_rTreeStore->get_path(treeIter), false/*open_all*/);
            }
            Glib::RefPtr<Gsv::Buffer> rTextBuffer = (*treeIter).get_value(_columns.rColTextBuffer);
            Gtk::TextIter textIter = rTextBuffer->get_iter_at_offset(CtApp::P_ctCfg->cursorPosition);
            if (static_cast<bool>(textIter))
            {
                rTextBuffer->place_cursor(textIter);
                pTextView->scroll_to(textIter, CtTextView::TEXT_SCROLL_MARGIN);
            }
        }
    }
    if (!treeSelFromConfig)
    {
        Gtk::TreeIter treeIter = _rTreeStore->get_iter("0");
        if (static_cast<bool>(treeIter))
        {
            pTreeView->set_cursor(_rTreeStore->get_path(treeIter));
        }
    }
}

void CtTreeStore::viewConnect(Gtk::TreeView* pTreeView)
{
    pTreeView->set_model(_rTreeStore);
}

void CtTreeStore::viewAppendColumns(Gtk::TreeView* pTreeView)
{
    Gtk::TreeView::Column* pColumns = Gtk::manage(new Gtk::TreeView::Column(""));
    pColumns->pack_start(_columns.rColPixbuf, /*expand=*/false);
    pColumns->pack_start(_columns.colNodeName);
    pColumns->pack_start(_columns.rColPixbufAux, /*expand=*/false);
    pTreeView->append_column(*pColumns);
}

bool CtTreeStore::readNodesFromFilepath(const char* filepath, const bool isImport, const Gtk::TreeIter* pParentIter)
{
    bool retOk{false};
    CtDocType docType = CtMiscUtil::getDocType(filepath);
    CtDocRead* pCtDocRead{nullptr};
    if (CtDocType::XML == docType)
    {
        pCtDocRead = new CtXmlRead(filepath, nullptr);
    }
    else if (CtDocType::SQLite == docType)
    {
        pCtDocRead = new CtSQLiteRead(filepath);
    }
    if (pCtDocRead != nullptr)
    {
        pCtDocRead->signalAddBookmark.connect(sigc::mem_fun(this, &CtTreeStore::onRequestAddBookmark));
        pCtDocRead->signalAppendNode.connect(sigc::mem_fun(this, &CtTreeStore::onRequestAppendNode));
        pCtDocRead->treeWalk(pParentIter);
        if (!isImport && (CtDocType::SQLite == docType))
        {
            _pCtSQLiteRead = dynamic_cast<CtSQLiteRead*>(pCtDocRead);
        }
        else
        {
            delete pCtDocRead;
        }
        retOk = true;
    }
    return retOk;
}

Glib::RefPtr<Gdk::Pixbuf> CtTreeStore::_getNodeIcon(int nodeDepth, std::string &syntax, guint32 customIconId)
{
    Glib::RefPtr<Gdk::Pixbuf> rPixbuf;

    if (0 != customIconId)
    {
        // customIconId
        rPixbuf = CtApp::R_icontheme->load_icon(CtConst::NODES_STOCKS.at(customIconId), CtConst::NODE_ICON_SIZE);
    }
    else if (CtConst::NODE_ICON_TYPE_NONE == CtApp::P_ctCfg->nodesIcons)
    {
        // NODE_ICON_TYPE_NONE
        rPixbuf = CtApp::R_icontheme->load_icon(CtConst::NODES_STOCKS.at(CtConst::NODE_ICON_NO_ICON_ID), CtConst::NODE_ICON_SIZE);
    }
    else if (CtStrUtil::isPgcharInPgcharSet(syntax.c_str(), CtConst::TEXT_SYNTAXES))
    {
        // text node
        if (CtConst::NODE_ICON_TYPE_CHERRY == CtApp::P_ctCfg->nodesIcons)
        {
            if (1 == CtConst::NODES_ICONS.count(nodeDepth))
            {
                rPixbuf = CtApp::R_icontheme->load_icon(CtConst::NODES_ICONS.at(nodeDepth), CtConst::NODE_ICON_SIZE);
            }
            else
            {
                rPixbuf = CtApp::R_icontheme->load_icon(CtConst::NODES_ICONS.at(-1), CtConst::NODE_ICON_SIZE);
            }
        }
        else
        {
            // NODE_ICON_TYPE_CUSTOM
            rPixbuf = CtApp::R_icontheme->load_icon(CtConst::NODES_STOCKS.at(CtApp::P_ctCfg->defaultIconText), CtConst::NODE_ICON_SIZE);
        }
    }
    else
    {
        // code node
        rPixbuf = CtApp::R_icontheme->load_icon(CtConst::getStockIdForCodeType(syntax), CtConst::NODE_ICON_SIZE);
    }
    return rPixbuf;
}

Gtk::TreeIter CtTreeStore::appendNode(CtNodeData* pNodeData, const Gtk::TreeIter* pParentIter)
{
    Gtk::TreeIter newIter;
    //std::cout << pNodeData->name << std::endl;

    if (nullptr == pParentIter)
    {
        newIter = _rTreeStore->append();
    }
    else
    {
        newIter = _rTreeStore->append(static_cast<Gtk::TreeRow>(**pParentIter).children());
    }
    Gtk::TreeRow row = *newIter;
    row[_columns.rColPixbuf] = _getNodeIcon(_rTreeStore->iter_depth(newIter), pNodeData->syntax, pNodeData->customIconId);
    row[_columns.colNodeName] = pNodeData->name;
    row[_columns.rColTextBuffer] = pNodeData->rTextBuffer;
    row[_columns.colNodeUniqueId] = pNodeData->nodeId;
    row[_columns.colSyntaxHighlighting] = pNodeData->syntax;
    //row[_columns.colNodeSequence] = ;
    row[_columns.colNodeTags] = pNodeData->tags;
    row[_columns.colNodeRO] = pNodeData->isRO;
    //row[_columns.rColPixbufAux] = ;
    row[_columns.colCustomIconId] = pNodeData->customIconId;
    row[_columns.colWeight] = _getPangoWeight(pNodeData->isBold);
    //row[_columns.colForeground] = ;
    row[_columns.colTsCreation] = pNodeData->tsCreation;
    row[_columns.colTsLastSave] = pNodeData->tsLastSave;
    row[_columns.colAnchoredWidgets] = pNodeData->anchoredWidgets;
    return newIter;
}

void CtTreeStore::onRequestAddBookmark(gint64 nodeId)
{
    _bookmarks.insert(nodeId);
}

guint16 CtTreeStore::_getPangoWeight(bool isBold)
{
    return isBold ? PANGO_WEIGHT_HEAVY : PANGO_WEIGHT_NORMAL;
}

Gtk::TreeIter CtTreeStore::onRequestAppendNode(CtNodeData* pNodeData, const Gtk::TreeIter* pParentIter)
{
    return appendNode(pNodeData, pParentIter);
}

Glib::RefPtr<Gsv::Buffer> CtTreeStore::_getNodeTextBuffer(const Gtk::TreeIter& treeIter)
{
    Glib::RefPtr<Gsv::Buffer> rRetTextBuffer{nullptr};
    if (treeIter)
    {
        Gtk::TreeRow treeRow = *treeIter;
        rRetTextBuffer = treeRow.get_value(_columns.rColTextBuffer);
        if (!rRetTextBuffer)
        {
            // SQLite text buffer not yet populated
            assert(nullptr != _pCtSQLiteRead);
            std::list<CtAnchoredWidget*> anchoredWidgetList = treeRow.get_value(_columns.colAnchoredWidgets);
            rRetTextBuffer = _pCtSQLiteRead->getTextBuffer(treeRow.get_value(_columns.colSyntaxHighlighting),
                                                           anchoredWidgetList,
                                                           treeRow.get_value(_columns.colNodeUniqueId));
            treeRow.set_value(_columns.colAnchoredWidgets, anchoredWidgetList);
            treeRow.set_value(_columns.rColTextBuffer, rRetTextBuffer);
        }
    }
    return rRetTextBuffer;
}

void CtTreeStore::applyTextBufferToCtTextView(const Gtk::TreeIter& treeIter, CtTextView* pTextView)
{
    if (!treeIter)
    {
        std::cerr << "!! treeIter" << std::endl;
        return;
    }
    Gtk::TreeRow treeRow = *treeIter;
    std::cout << treeRow.get_value(_columns.colNodeName) << std::endl;
    Glib::RefPtr<Gsv::Buffer> rTextBuffer = _getNodeTextBuffer(treeIter);
    pTextView->setupForSyntax(treeRow.get_value(_columns.colSyntaxHighlighting));
    pTextView->set_buffer(rTextBuffer);
    for (CtAnchoredWidget* pCtAnchoredWidget : treeRow.get_value(_columns.colAnchoredWidgets))
    {
        Glib::RefPtr<Gtk::TextChildAnchor> rChildAnchor = pCtAnchoredWidget->getTextChildAnchor();
        if (rChildAnchor)
        {
            if (0 == rChildAnchor->get_widgets().size())
            {
                Gtk::TextIter textIter = rTextBuffer->get_iter_at_child_anchor(rChildAnchor);
                pTextView->add_child_at_anchor(*pCtAnchoredWidget, rChildAnchor);
                pCtAnchoredWidget->applyWidthHeight(pTextView->get_allocation().get_width());
            }
            else
            {
                // this happens if we click on a node that is already selected, not an issue
                // we simply must not add the same widget to the anchor again
            }
        }
        else
        {
            std::cerr << "!! rChildAnchor" << std::endl;
        }
    }
    pTextView->show_all();
    pTextView->grab_focus();
}
