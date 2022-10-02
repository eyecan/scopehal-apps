/***********************************************************************************************************************
*                                                                                                                      *
* glscopeclient                                                                                                        *
*                                                                                                                      *
* Copyright (c) 2012-2022 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief Implementation of MainWindow
 */
#include "ngscopeclient.h"
#include "MainWindow.h"

#include "DemoOscilloscope.h"
#include "RemoteBridgeOscilloscope.h"

//Dock builder API is not yet public, so might change...
#include "imgui_internal.h"

//Dialogs
#include "AddGeneratorDialog.h"
#include "AddMultimeterDialog.h"
#include "AddPowerSupplyDialog.h"
#include "AddRFGeneratorDialog.h"
#include "AddScopeDialog.h"
#include "ChannelPropertiesDialog.h"
#include "FunctionGeneratorDialog.h"
#include "HistoryDialog.h"
#include "LogViewerDialog.h"
#include "MultimeterDialog.h"
#include "RFGeneratorDialog.h"
#include "SCPIConsoleDialog.h"
#include "TimebasePropertiesDialog.h"

using namespace std;

extern Event g_rerenderRequestedEvent;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

MainWindow::MainWindow(vk::raii::Queue& queue)
#ifdef _DEBUG
	: VulkanWindow("ngscopeclient [DEBUG BUILD]", queue)
#else
	: VulkanWindow("ngscopeclient", queue)
#endif
	, m_showDemo(true)
	, m_showPlot(false)
	, m_nextWaveformGroup(1)
	, m_session(this)
	, m_sessionClosing(false)
	, m_texmgr(m_imguiDescriptorPool)
	, m_needRender(false)
	, m_toneMapTime(0)
{
	LoadRecentInstrumentList();

	//Add default Latin-1 glyph ranges plus some Greek letters and symbols we use
	ImGuiIO& io = ImGui::GetIO();
	ImFontGlyphRangesBuilder builder;
	builder.AddRanges(io.Fonts->GetGlyphRangesGreek());
	builder.AddChar(L'°');

	//Build the range of glyphs we're using for the font
	ImVector<ImWchar> ranges;
	builder.BuildRanges(&ranges);

	//Load our fonts
	m_defaultFont = LoadFont("fonts/DejaVuSans.ttf", 13, ranges);
	m_monospaceFont = LoadFont("fonts/DejaVuSansMono.ttf", 13, ranges);

	//Done loading fonts, build the texture
	io.Fonts->Flags = ImFontAtlasFlags_NoMouseCursors;
	io.Fonts->Build();
	io.FontDefault = m_defaultFont;

	//Initialize command pool/buffer
	vk::CommandPoolCreateInfo poolInfo(
	vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		g_renderQueueType );
	m_cmdPool = make_unique<vk::raii::CommandPool>(*g_vkComputeDevice, poolInfo);

	vk::CommandBufferAllocateInfo bufinfo(**m_cmdPool, vk::CommandBufferLevel::ePrimary, 1);
	m_cmdBuffer = make_unique<vk::raii::CommandBuffer>(
		move(vk::raii::CommandBuffers(*g_vkComputeDevice, bufinfo).front()));

	if(g_hasDebugUtils)
	{
		g_vkComputeDevice->setDebugUtilsObjectNameEXT(
			vk::DebugUtilsObjectNameInfoEXT(
				vk::ObjectType::eCommandPool,
				reinterpret_cast<int64_t>(static_cast<VkCommandPool>(**m_cmdPool)),
				"MainWindow.m_cmdPool"));

		g_vkComputeDevice->setDebugUtilsObjectNameEXT(
			vk::DebugUtilsObjectNameInfoEXT(
				vk::ObjectType::eCommandBuffer,
				reinterpret_cast<int64_t>(static_cast<VkCommandBuffer>(**m_cmdBuffer)),
				"MainWindow.m_cmdBuffer"));
	}

	//Download imgui fonts
	m_cmdBuffer->begin({});
	ImGui_ImplVulkan_CreateFontsTexture(**m_cmdBuffer);
	m_cmdBuffer->end();
	SubmitAndBlock(*m_cmdBuffer, queue);
	ImGui_ImplVulkan_DestroyFontUploadObjects();

	//Load some textures
	//TODO: use preference to decide what size to make the icons
	m_texmgr.LoadTexture("clear-sweeps", FindDataFile("icons/24x24/clear-sweeps.png"));
	m_texmgr.LoadTexture("fullscreen-enter", FindDataFile("icons/24x24/fullscreen-enter.png"));
	m_texmgr.LoadTexture("fullscreen-exit", FindDataFile("icons/24x24/fullscreen-exit.png"));
	m_texmgr.LoadTexture("history", FindDataFile("icons/24x24/history.png"));
	m_texmgr.LoadTexture("refresh-settings", FindDataFile("icons/24x24/refresh-settings.png"));
	m_texmgr.LoadTexture("trigger-single", FindDataFile("icons/24x24/trigger-single.png"));
	m_texmgr.LoadTexture("trigger-force", FindDataFile("icons/24x24/trigger-single.png"));	//no dedicated icon yet
	m_texmgr.LoadTexture("trigger-start", FindDataFile("icons/24x24/trigger-start.png"));
	m_texmgr.LoadTexture("trigger-stop", FindDataFile("icons/24x24/trigger-stop.png"));

	m_texmgr.LoadTexture("warning", FindDataFile("icons/48x48/dialog-warning-2.png"));
}

MainWindow::~MainWindow()
{
	m_cmdBuffer = nullptr;
	m_cmdPool = nullptr;

	CloseSession();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Session termination

void MainWindow::CloseSession()
{
	LogTrace("Closing session\n");
	LogIndenter li;

	SaveRecentInstrumentList();

	//Close background threads in our session before destroying views
	m_session.ClearBackgroundThreads();

	//Destroy waveform views
	LogTrace("Clearing views\n");
	for(auto g : m_waveformGroups)
		g->Clear();
	m_waveformGroups.clear();
	m_newWaveformGroups.clear();
	m_splitRequests.clear();
	m_groupsToClose.clear();

	//Clear any open dialogs before destroying the session.
	//This ensures that we have a nice well defined shutdown order.
	LogTrace("Clearing dialogs\n");
	m_logViewerDialog = nullptr;
	m_metricsDialog = nullptr;
	m_timebaseDialog = nullptr;
	m_historyDialog = nullptr;
	m_meterDialogs.clear();
	m_channelPropertiesDialogs.clear();
	m_generatorDialogs.clear();
	m_rfgeneratorDialogs.clear();
	m_dialogs.clear();

	//Clear the actual session object once all views / dialogs having handles to scopes etc have been destroyed
	m_session.Clear();

	LogTrace("Clear complete\n");

	m_sessionClosing = false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Add views for new instruments

string MainWindow::NameNewWaveformGroup()
{
	//TODO: avoid colliding, check if name is in use and skip if so
	int id = (m_nextWaveformGroup ++);
	return string("Waveform Group ") + to_string(id);
}

/**
	@brief Figure out what group to use for a newly added stream, based on unit compatibility etc
 */
shared_ptr<WaveformGroup> MainWindow::GetBestGroupForWaveform(StreamDescriptor /*stream*/)
{
	//If we have no waveform groups, make one
	//TODO: reject existing group if units are incompatible
	if(m_waveformGroups.empty())
	{
		//Make the group
		auto name = NameNewWaveformGroup();
		auto group = make_shared<WaveformGroup>(this, name);
		m_waveformGroups.push_back(group);

		//Group is newly created and not yet docked
		m_newWaveformGroups.push_back(group);
	}

	//Get the first compatible waveform group (may or may not be what we just created)
	//TODO: reject existing group if units are incompatible
	return *m_waveformGroups.begin();
}

void MainWindow::OnScopeAdded(Oscilloscope* scope)
{
	LogTrace("Oscilloscope \"%s\" added\n", scope->m_nickname.c_str());
	LogIndenter li;

	//Add areas to it
	//For now, one area per enabled channel
	vector<StreamDescriptor> streams;

	//Headless scope? Pick every channel.
	if( (dynamic_cast<RemoteBridgeOscilloscope*>(scope)) || (dynamic_cast<DemoOscilloscope*>(scope)) )
	{
		LogTrace("Headless scope, enabling every analog channel\n");
		for(size_t i=0; i<scope->GetChannelCount(); i++)
		{
			auto chan = scope->GetChannel(i);
			for(size_t j=0; j<chan->GetStreamCount(); j++)
			{
				if(chan->GetType(j) == Stream::STREAM_TYPE_ANALOG)
					streams.push_back(StreamDescriptor(chan, j));
			}
		}

		//Handle pure logic analyzers
		if(streams.empty())
		{
			LogTrace("No analog channels found. Must be a logic analyzer. Enabling every digital channel\n");

			for(size_t i=0; i<scope->GetChannelCount(); i++)
			{
				auto chan = scope->GetChannel(i);
				for(size_t j=0; j<chan->GetStreamCount(); j++)
				{
					if(chan->GetType(j) == Stream::STREAM_TYPE_DIGITAL)
						streams.push_back(StreamDescriptor(chan, j));
				}
			}
		}
	}

	//Use whatever was enabled when we connected
	else
	{
		for(size_t i=0; i<scope->GetChannelCount(); i++)
		{
			auto chan = scope->GetChannel(i);
			if(!chan->IsEnabled())
				continue;

			for(size_t j=0; j<chan->GetStreamCount(); j++)
				streams.push_back(StreamDescriptor(chan, j));
		}
		LogTrace("%zu streams were active when we connected\n", streams.size());

		//No streams? Grab the first one.
		if(streams.empty())
		{
			LogTrace("Enabling first channel\n");
			streams.push_back(StreamDescriptor(scope->GetChannel(0), 0));
		}
	}

	//Add waveform areas for the streams
	for(auto s : streams)
	{
		auto group = GetBestGroupForWaveform(s);
		auto area = make_shared<WaveformArea>(s, group, this);
		group->AddArea(area);
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rendering

void MainWindow::Render()
{
	if(m_sessionClosing)
	{
		m_renderQueue.waitIdle();
		CloseSession();
	}

	VulkanWindow::Render();
}

void MainWindow::DoRender(vk::raii::CommandBuffer& /*cmdBuf*/)
{

}

/**
	@brief Run the tone-mapping shader on all of our waveforms

	Called by Session::CheckForWaveforms() at the start of each frame if new data is ready to render
 */
void MainWindow::ToneMapAllWaveforms(vk::raii::CommandBuffer& cmdbuf)
{
	double start = GetTime();

	m_cmdBuffer->begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

	for(auto group : m_waveformGroups)
		group->ToneMapAllWaveforms(cmdbuf);

	m_cmdBuffer->end();
	SubmitAndBlock(*m_cmdBuffer, m_renderQueue);

	double dt = GetTime() - start;
	m_toneMapTime = dt * FS_PER_SECOND;
}

void MainWindow::RenderWaveformTextures(
	vk::raii::CommandBuffer& cmdbuf,
	vector<shared_ptr<DisplayedChannel> >& channels)
{
	for(auto group : m_waveformGroups)
		group->RenderWaveformTextures(cmdbuf, channels);
}

void MainWindow::RenderUI()
{
	m_needRender = false;

	//Keep references to all of our waveform textures until next frame
	//Any groups we're closing will be destroyed at the start of that frame, once rendering has finished
	for(auto g : m_waveformGroups)
		g->ReferenceWaveformTextures();

	//Destroy all waveform groups we were asked to close
	//Block until all background processing completes to ensure no command buffers are still pending
	if(!m_groupsToClose.empty())
	{
		g_vkComputeDevice->waitIdle();
		m_groupsToClose.clear();
	}

	//See if we have new waveform data to look at.
	//If we got one, highlight the new waveform in history
	if(m_session.CheckForWaveforms(*m_cmdBuffer))
	{
		if(m_historyDialog != nullptr)
			m_historyDialog->UpdateSelectionToLatest();
	}

	//Menu for main window
	MainMenu();
	Toolbar();

	//Docking area to put all of the groups in
	DockingArea();

	//Waveform groups
	{
		lock_guard<recursive_mutex> lock(m_session.GetWaveformDataMutex());
		for(size_t i=0; i<m_waveformGroups.size(); i++)
		{
			auto group = m_waveformGroups[i];
			if(!group->Render())
			{
				LogTrace("Closing waveform group %s (i=%zu)\n", group->GetTitle().c_str(), i);
				group->Clear();
				m_groupsToClose.push_back(i);
			}
		}
		for(ssize_t i = static_cast<ssize_t>(m_groupsToClose.size())-1; i >= 0; i--)
			m_waveformGroups.erase(m_waveformGroups.begin() + m_groupsToClose[i]);
	}

	//Dialog boxes
	set< shared_ptr<Dialog> > dlgsToClose;
	for(auto& dlg : m_dialogs)
	{
		if(!dlg->Render())
			dlgsToClose.emplace(dlg);
	}
	for(auto& dlg : dlgsToClose)
		OnDialogClosed(dlg);

	//If we had a history dialog, check if we changed the selection
	if( (m_historyDialog != nullptr) && (m_historyDialog->PollForSelectionChanges()))
	{
		LogTrace("history selection changed\n");
		m_historyDialog->LoadHistoryFromSelection(m_session);
		m_needRender = true;
	}

	if(m_needRender)
		g_rerenderRequestedEvent.Signal();

	//DEBUG: draw the demo windows
	ImGui::ShowDemoWindow(&m_showDemo);
	//ImPlot::ShowDemoWindow(&m_showPlot);
}

void MainWindow::Toolbar()
{
	//Toolbar should be at the top of the main window.
	//Update work area size so docking area doesn't include the toolbar rectangle
	auto viewport = ImGui::GetMainViewport();
	auto toolbarHeight = ImGui::GetFontSize() * 2.5;
	m_workPos = ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + toolbarHeight);
	m_workSize = ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - toolbarHeight);
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, toolbarHeight));

	//Make the toolbar window
	auto wflags =
		ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoCollapse;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	bool open = true;
	ImGui::Begin("toolbar", &open, wflags);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));

	//Do the actual toolbar buttons
	ToolbarButtons();

	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);

	ImGui::End();
}

void MainWindow::ToolbarButtons()
{
	auto sz = 24;//ImGui::GetFontSize() * 2;
	ImVec2 buttonsize(sz, sz);

	//Trigger button group
	if(ImGui::ImageButton("trigger-start", GetTexture("trigger-start"), buttonsize))
		m_session.ArmTrigger(Session::TRIGGER_TYPE_NORMAL);
	Dialog::Tooltip("Arm the trigger in normal mode");

	ImGui::SameLine(0.0, 0.0);
	if(ImGui::ImageButton("trigger-single", GetTexture("trigger-single"), buttonsize))
		m_session.ArmTrigger(Session::TRIGGER_TYPE_SINGLE);
	Dialog::Tooltip("Arm the trigger in one-shot mode");

	ImGui::SameLine(0.0, 0.0);
	if(ImGui::ImageButton("trigger-force", GetTexture("trigger-force"), buttonsize))
		m_session.ArmTrigger(Session::TRIGGER_TYPE_FORCED);
	Dialog::Tooltip("Acquire a waveform immediately, ignoring the trigger condition");

	ImGui::SameLine(0.0, 0.0);
	if(ImGui::ImageButton("trigger-stop", GetTexture("trigger-stop"), buttonsize))
		m_session.StopTrigger();
	Dialog::Tooltip("Stop acquiring waveforms");

	//History selector
	bool hasHist = (m_historyDialog != nullptr);
	ImGui::SameLine();
	if(hasHist)
		ImGui::BeginDisabled();
	if(ImGui::ImageButton("history", GetTexture("history"), buttonsize))
	{
		m_historyDialog = make_shared<HistoryDialog>(m_session.GetHistory());
		AddDialog(m_historyDialog);
	}
	if(hasHist)
		ImGui::EndDisabled();
	Dialog::Tooltip("Show waveform history window");

	//Refresh scope settings
	ImGui::SameLine();
	if(ImGui::ImageButton("refresh-settings", GetTexture("refresh-settings"), buttonsize))
		LogDebug("refresh settings\n");
	Dialog::Tooltip(
		"Flush PC-side cached instrument state and reload configuration from the instrument.\n\n"
		"This will cause a brief slowdown of the application, but can be used to re-sync when\n"
		"changes are made on the instrument front panel that ngscopeclient does not detect."
		);

	//View settings
	ImGui::SameLine();
	if(ImGui::ImageButton("clear-sweeps", GetTexture("clear-sweeps"), buttonsize))
		LogDebug("clear-sweeps\n");
	Dialog::Tooltip("Clear waveform persistence, eye patterns, and accumulated statistics");

	//Fullscreen toggle
	ImGui::SameLine(0.0, 0.0);
	if(m_fullscreen)
	{
		if(ImGui::ImageButton("fullscreen-exit", GetTexture("fullscreen-exit"), buttonsize))
			SetFullscreen(false);
		Dialog::Tooltip("Leave fullscreen mode");
	}
	else
	{
		if(ImGui::ImageButton("fullscreen-enter", GetTexture("fullscreen-enter"), buttonsize))
			SetFullscreen(true);
		Dialog::Tooltip("Enter fullscreen mode");
	}
}

void MainWindow::OnDialogClosed(const std::shared_ptr<Dialog>& dlg)
{
	//Multimeter dialogs are stored in a separate list
	auto meterDlg = dynamic_pointer_cast<MultimeterDialog>(dlg);
	if(meterDlg)
		m_meterDialogs.erase(meterDlg->GetMeter());

	//Function generator dialogs are stored in a separate list
	auto genDlg = dynamic_pointer_cast<FunctionGeneratorDialog>(dlg);
	if(genDlg)
		m_generatorDialogs.erase(genDlg->GetGenerator());

	//RF generator dialogs are stored in a separate list
	auto rgenDlg = dynamic_pointer_cast<RFGeneratorDialog>(dlg);
	if(rgenDlg)
		m_rfgeneratorDialogs.erase(rgenDlg->GetGenerator());

	if(m_logViewerDialog == dlg)
		m_logViewerDialog = nullptr;

	if(m_timebaseDialog == dlg)
		m_timebaseDialog = nullptr;

	auto conDlg = dynamic_pointer_cast<SCPIConsoleDialog>(dlg);
	if(conDlg)
		m_scpiConsoleDialogs.erase(conDlg->GetInstrument());

	auto chanDlg = dynamic_pointer_cast<ChannelPropertiesDialog>(dlg);
	if(chanDlg)
		m_channelPropertiesDialogs.erase(chanDlg->GetChannel());

	m_dialogs.erase(dlg);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Waveform views etc

void MainWindow::DockingArea()
{
	//Provide a space we can dock windows into
	auto viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(m_workPos);
	ImGui::SetNextWindowSize(m_workSize);
	ImGui::SetNextWindowViewport(viewport->ID);

	ImGuiWindowFlags host_window_flags = 0;
	host_window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;
	host_window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

	char label[32];
	ImFormatString(label, IM_ARRAYSIZE(label), "DockSpaceViewport_%08X", viewport->ID);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin(label, NULL, host_window_flags);
	ImGui::PopStyleVar(3);

	auto dockspace_id = ImGui::GetID("DockSpace");

	//Handle splitting of existing waveform groups
	if(!m_splitRequests.empty())
	{
		LogTrace("Processing split request\n");

		for(auto request : m_splitRequests)
		{
			//Get the window for the group
			auto window = ImGui::FindWindowByName(request.m_group->GetTitle().c_str());
			if(!window)
			{
				//Not sure if this is possible? Haven't seen it yet
				LogWarning("Window is null (TODO handle this)\n");
				continue;
			}
			if(!window->DockNode)
			{
				//If we get here, we dragged into a floating window without a dock space in it
				LogWarning("Dock node is null (TODO handle this)\n");
				continue;
			}

			auto dockid = window->DockId;

			//Split the existing node
			ImGuiID idA;
			ImGuiID idB;
			ImGui::DockBuilderSplitNode(dockid, request.m_direction, 0.5, &idA, &idB);
			auto node = ImGui::DockBuilderGetNode(idA);

			//Create a new waveform group and dock it into the new space
			auto group = make_shared<WaveformGroup>(this, NameNewWaveformGroup());
			m_waveformGroups.push_back(group);
			ImGui::DockBuilderDockWindow(group->GetTitle().c_str(), node->ID);

			//Add a new waveform area for our stream to the new group
			auto area = make_shared<WaveformArea>(request.m_stream, group, this);
			group->AddArea(area);
		}

		//Finish up
		ImGui::DockBuilderFinish(dockspace_id);

		m_splitRequests.clear();
	}

	//Handle newly created waveform groups
	//Do not do this the same frame as split requests
	else if(!m_newWaveformGroups.empty())
	{
		LogTrace("Processing newly added waveform group\n");

		//Find the top/leftmost leaf node in the docking tree
		auto topNode = ImGui::DockBuilderGetNode(dockspace_id);
		if(topNode == nullptr)
		{
			LogError("Top dock node is null when adding new waveform group\n");
			return;
		}

		//Traverse down the top/left of the tree as long as such a node exists
		auto node = topNode;
		while(node->ChildNodes[0])
			node = node->ChildNodes[0];

		//See if the node has children in it
		if(!node->Windows.empty())
		{
			LogTrace("Windows already in node, splitting it\n");
			ImGuiID idLeft;
			ImGuiID idRight;

			ImGui::DockBuilderSplitNode(node->ID, ImGuiDir_Up, 0.5, &idLeft, &idRight);
			node = ImGui::DockBuilderGetNode(idLeft);
		}

		//Dock new waveform groups by default
		for(auto& g : m_newWaveformGroups)
			ImGui::DockBuilderDockWindow(g->GetTitle().c_str(), node->ID);

		//Finish up
		ImGui::DockBuilderFinish(dockspace_id);

		//Everything pending has been docked, no need to do anything with them in the future
		m_newWaveformGroups.clear();
	}

	ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), /*dockspace_flags*/0, /*window_class*/nullptr);
	ImGui::End();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Other GUI handlers

/**
	@brief Returns true if a channel is being dragged from any WaveformArea within this window
 */
bool MainWindow::IsChannelBeingDragged()
{
	for(auto group : m_waveformGroups)
	{
		if(group->IsChannelBeingDragged())
			return true;
	}
	return false;
}

/**
	@brief Returns the channel being dragged, if one exists
 */
StreamDescriptor MainWindow::GetChannelBeingDragged()
{
	for(auto group : m_waveformGroups)
	{
		auto stream = group->GetChannelBeingDragged();
		if(stream)
			return stream;
	}
	return StreamDescriptor(nullptr, 0);
}

void MainWindow::ShowTimebaseProperties()
{
	if(m_timebaseDialog != nullptr)
		return;

	m_timebaseDialog = make_shared<TimebasePropertiesDialog>(&m_session);
	AddDialog(m_timebaseDialog);
}

void MainWindow::ShowChannelProperties(OscilloscopeChannel* channel)
{
	LogTrace("Show properties for %s\n", channel->GetHwname().c_str());
	LogIndenter li;

	if(m_channelPropertiesDialogs.find(channel) != m_channelPropertiesDialogs.end())
	{
		LogTrace("Properties dialog is already open, no action required\n");
		return;
	}

	//Dialog wasn't already open, create it
	auto dlg = make_shared<ChannelPropertiesDialog>(channel);
	m_channelPropertiesDialogs[channel] = dlg;
	AddDialog(dlg);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Recent instruments

void MainWindow::LoadRecentInstrumentList()
{
	try
	{
		auto docs = YAML::LoadAllFromFile(m_preferences.GetConfigDirectory() + "/recent.yml");
		if(docs.empty())
			return;
		auto node = docs[0];

		for(auto it : node)
		{
			auto inst = it.second;
			m_recentInstruments[inst["path"].as<string>()] = inst["timestamp"].as<long long>();
		}
	}
	catch(const YAML::BadFile& ex)
	{
		LogDebug("Unable to open recently used instruments file\n");
		return;
	}

}

void MainWindow::SaveRecentInstrumentList()
{
	auto path = m_preferences.GetConfigDirectory() + "/recent.yml";
	FILE* fp = fopen(path.c_str(), "w");

	for(auto it : m_recentInstruments)
	{
		auto nick = it.first.substr(0, it.first.find(":"));
		fprintf(fp, "%s:\n", nick.c_str());
		fprintf(fp, "    path: \"%s\"\n", it.first.c_str());
		fprintf(fp, "    timestamp: %ld\n", it.second);
	}

	fclose(fp);
}

void MainWindow::AddToRecentInstrumentList(SCPIInstrument* inst)
{
	if(inst == nullptr)
		return;

	auto now = time(NULL);

	auto connectionString =
		inst->m_nickname + ":" +
		inst->GetDriverName() + ":" +
		inst->GetTransportName() + ":" +
		inst->GetTransportConnectionString();
	m_recentInstruments[connectionString] = now;

	//Delete anything old
	//TODO: have a preference for this
	const int maxRecentInstruments = 20;
	while(m_recentInstruments.size() > maxRecentInstruments)
	{
		string oldestPath = "";
		time_t oldestTime = now;

		for(auto it : m_recentInstruments)
		{
			if(it.second < oldestTime)
			{
				oldestTime = it.second;
				oldestPath = it.first;
			}
		}

		m_recentInstruments.erase(oldestPath);
	}
}

/**
	@brief Helper function for creating a transport and printing an error if the connection is unsuccessful
 */
SCPITransport* MainWindow::MakeTransport(const string& trans, const string& args)
{
	//Create the transport
	auto transport = SCPITransport::CreateTransport(trans, args);
	if(transport == nullptr)
	{
		ShowErrorPopup(
			"Transport error",
			"Failed to create transport of type \"" + trans + "\"");
		return nullptr;
	}

	//Make sure we connected OK
	if(!transport->IsConnected())
	{
		delete transport;
		ShowErrorPopup("Connection error", "Failed to connect to \"" + args + "\"");
		return nullptr;
	}

	return transport;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Dialog helpers

/**
	@brief Opens the error popup
 */
void MainWindow::ShowErrorPopup(const string& title, const string& msg)
{
	ImGui::OpenPopup(title.c_str());
	m_errorPopupTitle = title;
	m_errorPopupMessage = msg;
}

/**
	@brief Popup message when we fail to connect
 */
void MainWindow::RenderErrorPopup()
{
	if(ImGui::BeginPopupModal(m_errorPopupTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted(m_errorPopupMessage.c_str());
		ImGui::Separator();
		if(ImGui::Button("OK"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

/**
	@brief Closes the function generator dialog, if we have one
 */
void MainWindow::RemoveFunctionGenerator(SCPIFunctionGenerator* gen)
{
	auto it = m_generatorDialogs.find(gen);
	if(it != m_generatorDialogs.end())
	{
		m_generatorDialogs.erase(gen);
		m_dialogs.erase(it->second);
	}
}
