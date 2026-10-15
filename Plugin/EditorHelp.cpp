/**
 * @file EditorHelp.cpp
 * @brief The help page, full screen and the update notice (23.09.2026).
 *
 * The help is the manual's prose (Tools/manual/chapters.txt, compiled in as PhospheneHelpData) arranged by
 * topic, plus two topics made on the spot: the parameters of the tab the help was opened from -- name, range,
 * default and the value it has now, read from the same descriptors that built the tab -- and the update
 * check. Noctuary's help page was the model: a topic list on the left, the text beside it, F1 to open it.
 * @author Rene Weller
 */
#include "PluginEditor.h"
#include "PhospheneHelpData.h"
#include <cmath>

using namespace phos;
using namespace phosui;

namespace
{
/**
 * @brief chapters.txt as (name, text) pairs: paragraphs joined, indented lines kept as they stand.
 * @return (name, text) pairs
 */
std::vector<std::pair<juce::String, juce::String>> readChapters()
{
    std::vector<std::pair<juce::String, juce::String>> out;
    const juce::String                                 all =
        juce::String::fromUTF8( PhospheneHelpData::chapters_txt, PhospheneHelpData::chapters_txtSize );
    juce::StringArray lines = juce::StringArray::fromLines( all );
    juce::String      name, textOf, para;
    auto              flushPara = [&]
    {
        if ( para.isNotEmpty() )
        {
            textOf << para.trimEnd() << "\n\n";
            para.clear();
        }
    };
    auto flushChapter = [&]
    {
        flushPara();
        if ( name.isNotEmpty() ) out.emplace_back( name, textOf.trimEnd() );
        textOf.clear();
    };
    for ( const juce::String &raw : lines )
    {
        const juce::String l = raw.trimEnd();
        if ( l.startsWith( "#" ) ) continue;
        if ( l.startsWith( "== " ) && l.endsWith( " ==" ) )
        {
            flushChapter();
            name = l.substring( 3, l.length() - 3 ).trim();
            continue;
        }
        if ( name.isEmpty() ) continue;
        if ( l.isEmpty() )
        {
            flushPara();
            continue;
        }
        if ( l.startsWith( "    " ) )
        {
            flushPara();
            textOf << l << "\n";
            continue;
        }
        if ( para.isNotEmpty() ) para << " ";
        para << l.trim();
    }
    flushChapter();
    return out;
}

/**
 * @brief A parameter's range in words, for the parameter topic.
 * @param[in] d  the parameter's description
 * @return its range in words
 */
juce::String rangeOf( const ParamDesc &d )
{
    if ( d.m_curve == CurveE::TOGGLE ) return "on / off";
    if ( d.m_curve == CurveE::CHOICE && d.m_choices != nullptr )
    {
        juce::StringArray c;
        for ( int i = 0; i <= static_cast<int>( std::lround( d.m_maxValue ) ); ++i ) c.add( d.m_choices[i] );
        return c.joinIntoString( " / " );
    }
    auto num = [&]( float v )
    { return juce::String( v, std::fabs( v ) >= 100.0f || d.m_curve == CurveE::INT ? 0 : 2 ); };
    return num( d.m_minValue ) + " .. " + num( d.m_maxValue ) +
           ( d.m_unit != nullptr && d.m_unit[0] != 0 ? juce::String( " " ) + d.m_unit : juce::String() );
}
}   // namespace

// ==================================================================== HelpView

HelpView::HelpView( PhospheneProcessor &proc ) : m_proc( proc )
{
    for ( const auto &[chapter, prose] : readChapters() )
    {
        m_names.add( chapter );
        m_texts.add( prose );
    }
    m_paramTopic = m_names.size();
    m_names.add( "Parameters on this tab" );
    m_texts.add( {} );
    m_updateTopic = m_names.indexOf( "Updates" );
    m_flowTopic = m_names.indexOf( "Signal flow" );
    addChildComponent( m_flow );

    m_list.setModel( this );
    m_list.setRowHeight( 26 );
    m_list.setColour( juce::ListBox::backgroundColourId, juce::Colours::transparentBlack );
    addAndMakeVisible( m_list );
    m_text.setMultiLine( true, true );
    m_text.setReadOnly( true );
    m_text.setCaretVisible( false );
    m_text.setScrollbarsShown( true );
    m_text.setFont( body( 15.0f ) );
    m_text.setColour( juce::TextEditor::backgroundColourId, Card.withAlpha( 0.6f ) );
    m_text.setColour( juce::TextEditor::outlineColourId, Edge );
    m_text.setColour( juce::TextEditor::textColourId, Text );
    m_text.setIndents( 14, 12 );
    addAndMakeVisible( m_text );

    // The update check's controls, shown with the "Updates" topic.
    const juce::File state = m_proc.userFolder().getChildFile( "update.txt" );
    m_autoCheck.setToggleState( phosui::UpdateCheck::enabled( state ), juce::dontSendNotification );
    m_autoCheck.onClick = [this, state] { phosui::UpdateCheck::setEnabled( state, m_autoCheck.getToggleState() ); };
    m_checkNow.onClick = [this, state]
    {
        m_updates->startIfDue( state, true );
        m_updateLine.setText( "asking GitHub ...", juce::dontSendNotification );
    };
    m_openRelease.onClick = [this]
    {
        const juce::String url = m_updates->result().m_url;
        juce::URL( url.isNotEmpty() ? url : juce::String( "https://github.com/reneweller-coding/Phosphene/releases" ) )
            .launchInDefaultBrowser();
    };
    m_updateLine.setColour( juce::Label::textColourId, Dim );
    for ( juce::Component *c :
          { static_cast<juce::Component *>( &m_autoCheck ), static_cast<juce::Component *>( &m_checkNow ),
            static_cast<juce::Component *>( &m_openRelease ), static_cast<juce::Component *>( &m_updateLine ) } )
        addChildComponent( c );
    m_list.updateContent();
    m_list.selectRow( 0 );
}

juce::String HelpView::topicText( int index ) const
{
    if ( index < 0 || index >= m_texts.size() ) return {};
    juce::String s = m_texts[index];
    if ( index == m_updateTopic )
    {
        s << "\n\nThis is Phosphene " << JucePlugin_VersionString << ".";
    }
    return s;
}

void HelpView::open( const juce::String &name, const juce::String &tabName, std::vector<int> paramIds )
{
    // The parameter topic: every parameter the tab shows, in the order it shows them.
    const ParamStore &p = m_proc.params();
    juce::String      s;
    s << "The parameters of the " << tabName << " tab, in the order the tab shows them: the name, the range, the "
      << "default and -- in brackets -- the value it has now. The key in front is the name a .phosset, phos_render "
      << "--set and MIDI learn use.\n\n";
    for ( int id : paramIds )
    {
        if ( id < 0 || id >= p.count() ) continue;
        const ParamDesc &d = p.desc( id );
        auto             shown = [&]( float v )
        {
            if ( d.m_curve == CurveE::TOGGLE ) return juce::String( v >= 0.5f ? "on" : "off" );
            if ( d.m_curve == CurveE::CHOICE && d.m_choices != nullptr )
                return juce::String( d.m_choices[juce::jlimit( 0, static_cast<int>( std::lround( d.m_maxValue ) ),
                                                               static_cast<int>( std::lround( v ) ) )] );
            return juce::String( v, std::fabs( v ) >= 100.0f ? 0 : 2 );
        };
        s << p.key( id ) << "\n    " << d.m_name << ":  " << rangeOf( d ) << ",  default "
          << shown( p.defaultValue( id ) ) << "  (now " << shown( p.get( id ) ) << ")\n";
    }
    if ( paramIds.empty() ) s << "This tab has no parameters of its own.";
    m_texts.set( m_paramTopic, s );
    m_names.set( m_paramTopic, "Parameters: " + tabName );
    m_list.updateContent();
    m_list.repaintRow( m_paramTopic );
    int row = m_names.indexOf( name );
    if ( row < 0 ) row = m_names.indexOf( "About" );
    m_list.selectRow( juce::jmax( 0, row ) );
    selectedRowsChanged( m_list.getSelectedRow() );
}

void HelpView::paintListBoxItem( int row, juce::Graphics &g, int w, int h, bool selected )
{
    if ( row < 0 || row >= m_names.size() ) return;
    if ( selected )
    {
        g.setColour( Accent.withAlpha( 0.22f ) );
        g.fillRoundedRectangle( 2.0f, 1.0f, w - 4.0f, h - 2.0f, 5.0f );
    }
    g.setColour( selected ? Text : Dim );
    g.setFont( body( 14.0f ) );
    g.drawText( m_names[row], 12, 0, w - 16, h, juce::Justification::centredLeft, true );
}

void HelpView::selectedRowsChanged( int row )
{
    m_text.setText( topicText( row ), false );
    m_text.moveCaretToTop( false );
    const bool upd = row == m_updateTopic && m_updateTopic >= 0;
    for ( juce::Component *c :
          { static_cast<juce::Component *>( &m_autoCheck ), static_cast<juce::Component *>( &m_checkNow ),
            static_cast<juce::Component *>( &m_openRelease ), static_cast<juce::Component *>( &m_updateLine ) } )
        c->setVisible( upd );
    m_flow.setVisible( row == m_flowTopic && m_flowTopic >= 0 );
    if ( upd ) refreshUpdate();
    resized();
}

void HelpView::refreshUpdate()
{
    if ( !m_updateLine.isVisible() ) return;
    const phosui::UpdateCheck::Result r = m_updates->result();
    juce::String                      line = r.m_message.isNotEmpty() ? r.m_message : juce::String( "Not asked yet." );
    if ( r.m_asked && r.m_when.toMilliseconds() > 0 )
        line << "   (asked " << r.m_when.formatted( "%d.%m.%Y %H:%M" ) << ")";
    if ( line != m_updateLine.getText() ) m_updateLine.setText( line, juce::dontSendNotification );
    m_openRelease.setEnabled( true );
}

void HelpView::paint( juce::Graphics &g )
{
    g.setColour( GroupColour );
    g.fillRoundedRectangle( getLocalBounds().toFloat(), 8.0f );
    g.setColour( Text );
    g.setFont( title( 13.0f ) );
    g.drawText( "HELP", 22, 10, 120, 20, juce::Justification::centredLeft, false );
    g.setColour( Dim );
    g.setFont( body( 11.0f ) );
    g.drawText( "F1, Esc or Help closes it.  The same text is the manual's (Phosphene-Manual.pdf).", 90, 10,
                getWidth() - 110, 20, juce::Justification::centredLeft, false );
}

void HelpView::resized()
{
    juce::Rectangle<int> r = getLocalBounds().reduced( 14 ).withTrimmedTop( 24 );
    m_list.setBounds( r.removeFromLeft( 230 ) );
    r.removeFromLeft( 12 );
    if ( m_updateLine.isVisible() )
    {
        juce::Rectangle<int> row = r.removeFromBottom( 34 );
        m_autoCheck.setBounds( row.removeFromLeft( 260 ) );
        m_checkNow.setBounds( row.removeFromLeft( 120 ).reduced( 4, 2 ) );
        m_openRelease.setBounds( row.removeFromLeft( 190 ).reduced( 4, 2 ) );
        m_updateLine.setBounds( r.removeFromBottom( 26 ) );
        r.removeFromBottom( 6 );
    }
    if ( m_flow.isVisible() )
    {
        // The picture on top at its own proportions, the chapter's text under it.
        const int h = juce::jmin(
            juce::roundToInt( static_cast<float>( r.getHeight() ) * 0.68f ),
            juce::roundToInt( static_cast<float>( r.getWidth() ) * SignalFlow::CanvasH / SignalFlow::CanvasW ) );
        m_flow.setBounds( r.removeFromTop( h ) );
        r.removeFromTop( 8 );
    }
    m_text.setBounds( r.withWidth( juce::jmin( r.getWidth(), 900 ) ) );
}

// ==================================================================== the editor's side

void PhospheneEditor::showHelp( bool show )
{
    if ( m_help == nullptr ) return;
    if ( show )
    {
        // The topic of the tab that is open, and that tab's parameters.
        std::vector<int> ids;
        if ( auto *page = activePage() )
            for ( int g = 0; g < page->groupCount(); ++g )
                for ( int id : page->groupParams( g ) ) ids.push_back( id );
        const juce::String tabName = tabNames()[m_tab];
        m_help->open( tabName, tabName, std::move( ids ) );
    }
    m_help->setVisible( show );
    m_viewport.setVisible( !show );
    for ( auto *b : m_laneButtons ) b->setVisible( !show && m_tab == TAB_PERC );
    m_helpIcon.setToggleState( show, juce::dontSendNotification );
    layoutContent();
    m_content.repaint();
    if ( show ) m_help->grabKeyboardFocus();
}

void PhospheneEditor::toggleFullScreen()
{
    // Only the standalone has a window of its own; in a host the host owns it, and the button is hidden.
    auto *window = findParentComponentOfClass<juce::DocumentWindow>();
    if ( window == nullptr ) return;
    auto      &desktop = juce::Desktop::getInstance();
    const bool on = desktop.getKioskModeComponent() != window;
    desktop.setKioskModeComponent( on ? window : nullptr, false );
    grabKeyboardFocus();   // so F11 and Esc keep working in the new window state
}

void PhospheneEditor::parentHierarchyChanged()
{
    // As Noctuary: a maximise button beside the other two. Only the standalone has a DocumentWindow of its own;
    // in a host this finds nothing, which is right.
    //
    // Not here and now, but on the next turn of the message loop: this is called while JUCE's standalone window
    // is still putting its content in and is a few pixels big, and changing the title bar re-lays the window out
    // at that size -- which pushed the editor down to its minimum size (measured: 430 x 373 instead of the
    // design size, once the fixed aspect ratio no longer hid it).
    juce::MessageManager::callAsync(
        [safe = juce::Component::SafePointer<PhospheneEditor>( this )]
        {
            if ( safe == nullptr ) return;
            auto *window = safe->findParentComponentOfClass<juce::DocumentWindow>();
            if ( window != nullptr )
                window->setTitleBarButtonsRequired( juce::DocumentWindow::minimiseButton |
                                                        juce::DocumentWindow::maximiseButton |
                                                        juce::DocumentWindow::closeButton,
                                                    false );
            // The standalone's window back where it was left (15.10.2026; not while it photographs itself).
            if ( !safe->m_shooting ) safe->m_window.attach( *safe, "Phosphene" );
        } );
}
