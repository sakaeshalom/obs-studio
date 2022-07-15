#include "aja-card-manager.hpp"
#include "aja-common.hpp"
#include "aja-ui-props.hpp"
#include "aja-output.hpp"
#include "aja-routing.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <ajabase/common/timer.h>
#include <ajabase/system/systemtime.h>

#include <ajantv2/includes/ntv2card.h>
#include <ajantv2/includes/ntv2devicefeatures.h>

#include <atomic>
#include <stdlib.h>

// Log AJA Output video/audio delay and av-sync
// #define AJA_OUTPUT_STATS

static constexpr uint32_t kNumCardFrames = 3;
static const int64_t kDefaultStatPeriod = 3000000000;
static const int64_t kAudioSyncAdjust = 20000;

static void copy_audio_data(struct audio_data *src, struct audio_data *dst,
			    size_t size)
{
	if (src->data[0]) {
		dst->data[0] = (uint8_t *)bmemdup(src->data[0], size);
	}
}

static void free_audio_data(struct audio_data *frames)
{
	if (frames->data[0]) {
		bfree(frames->data[0]);
		frames->data[0] = NULL;
	}
	memset(frames, 0, sizeof(*frames));
}

static void copy_video_data(struct video_data *src, struct video_data *dst,
			    size_t size)
{
	if (src->data[0]) {
		dst->data[0] = (uint8_t *)bmemdup(src->data[0], size);
	}
}

static void free_video_frame(struct video_data *frame)
{
	if (frame->data[0]) {
		bfree(frame->data[0]);
		frame->data[0] = NULL;
	}

	memset(frame, 0, sizeof(*frame));
}

static void update_sdi_transport_and_sdi_transport_4k(obs_properties_t *props,
						      NTV2DeviceID device_id,
						      IOSelection io,
						      NTV2VideoFormat vf)
{
	// Update SDI Transport and SDI 4K Transport selections
	obs_property_t *sdi_trx_list =
		obs_properties_get(props, kUIPropSDITransport.id);
	obs_property_list_clear(sdi_trx_list);
	populate_sdi_transport_list(sdi_trx_list, io, device_id);
	obs_property_t *sdi_4k_trx_list =
		obs_properties_get(props, kUIPropSDITransport4K.id);
	obs_property_list_clear(sdi_4k_trx_list);
	populate_sdi_4k_transport_list(sdi_4k_trx_list);

	bool is_sdi = aja::IsIOSelectionSDI(io);
	obs_property_set_visible(sdi_trx_list, is_sdi);
	obs_property_set_visible(sdi_4k_trx_list,
				 is_sdi && NTV2_IS_4K_VIDEO_FORMAT(vf));
}

AJAOutput::AJAOutput(CNTV2Card *card, const std::string &cardID,
		     const std::string &outputID, UWord deviceIndex,
		     const NTV2DeviceID deviceID)
	: mCardID{cardID},
	  mOutputID{outputID},
	  mDeviceIndex{deviceIndex},
	  mDeviceID{deviceID},
	  mNumCardFrames{0},
	  mFirstCardFrame{0},
	  mLastCardFrame{0},
	  mWriteCardFrame{0},
	  mPlayCardFrame{0},
	  mPlayCardNext{0},
	  mFrameRateNum{0},
	  mFrameRateDen{0},
	  mVideoQueueFrames{0},
	  mVideoWriteFrames{0},
	  mVideoPlayFrames{0},
	  mCard{card},
	  mOutputProps{DEVICE_ID_NOTFOUND},
	  mTestPattern{},
	  mIsRunning{false},
	  mRunThread{},
	  mVideoLock{},
	  mRunThreadLock{},
	  mVideoQueue{},
	  mOBSOutput{nullptr},
	  mCrosspoints{}
{
	mVideoQueue = std::make_unique<VideoQueue>();
}

AJAOutput::~AJAOutput()
{
	if (mVideoQueue)
		mVideoQueue.reset();
}

CNTV2Card *AJAOutput::GetCard()
{
	return mCard;
}

void AJAOutput::Initialize(const OutputProps &props)
{
	// Specify the frame indices for the "on-air" frames on the card.
	// Starts at frame index corresponding to the output Channel * numFrames
	calculate_card_frame_indices(kNumCardFrames, mCard->GetDeviceID(),
				     props.Channel(), props.videoFormat,
				     props.pixelFormat);

	mCard->SetOutputFrame(props.Channel(), mWriteCardFrame);
	mCard->WaitForOutputVerticalInterrupt(props.Channel());
	const auto &cardFrameRate =
		GetNTV2FrameRateFromVideoFormat(props.videoFormat);
	ULWord fpsNum = 0;
	ULWord fpsDen = 0;
	GetFramesPerSecond(cardFrameRate, fpsNum, fpsDen);
	mFrameRateNum = fpsNum;
	mFrameRateDen = fpsDen;
	SetOutputProps(props);
}

void AJAOutput::SetOBSOutput(obs_output_t *output)
{
	mOBSOutput = output;
}

obs_output_t *AJAOutput::GetOBSOutput()
{
	return mOBSOutput;
}

void AJAOutput::SetOutputProps(const OutputProps &props)
{
	mOutputProps = props;
}

OutputProps AJAOutput::GetOutputProps() const
{
	return mOutputProps;
}

void AJAOutput::CacheConnections(const NTV2XptConnections &cnx)
{
	mCrosspoints.clear();
	mCrosspoints = cnx;
}

void AJAOutput::ClearConnections()
{
	for (auto &&xpt : mCrosspoints) {
		mCard->Connect(xpt.first, NTV2_XptBlack);
	}
	mCrosspoints.clear();
}

void AJAOutput::GenerateTestPattern(NTV2VideoFormat vf, NTV2PixelFormat pf,
				    NTV2TestPatternSelect pattern)
{
	NTV2VideoFormat vid_fmt = vf;
	NTV2PixelFormat pix_fmt = pf;

	if (vid_fmt == NTV2_FORMAT_UNKNOWN)
		vid_fmt = NTV2_FORMAT_720p_5994;
	if (pix_fmt == NTV2_FBF_INVALID)
		pix_fmt = kDefaultAJAPixelFormat;

	NTV2FormatDesc fd(vid_fmt, pix_fmt, NTV2_VANCMODE_OFF);
	auto bufSize = fd.GetTotalRasterBytes();

	// Raster size changed, regenerate pattern
	if (bufSize != mTestPattern.size()) {
		mTestPattern.clear();
		mTestPattern.resize(bufSize);

		NTV2TestPatternGen gen;
		gen.DrawTestPattern(pattern, fd.GetRasterWidth(),
				    fd.GetRasterHeight(), pix_fmt,
				    mTestPattern);
	}

	if (mTestPattern.size() == 0) {
		blog(LOG_DEBUG,
		     "AJAOutput::GenerateTestPattern: Error generating test pattern!");
		return;
	}

	auto outputChannel = mOutputProps.Channel();

	mCard->SetOutputFrame(outputChannel, mWriteCardFrame);

	mCard->DMAWriteFrame(
		mWriteCardFrame,
		reinterpret_cast<ULWord *>(&mTestPattern.data()[0]),
		static_cast<ULWord>(mTestPattern.size()));
}

void AJAOutput::QueueVideoFrame(struct video_data *frame, size_t size)
{
	const std::lock_guard<std::mutex> lock(mVideoLock);

	VideoFrame vf;
	vf.frame = *frame;
	vf.size = size;

	if (mVideoQueue->size() > kVideoQueueMaxSize) {
		auto &front = mVideoQueue->front();
		free_video_frame(&front.frame);
		mVideoQueue->pop_front();
	}

	copy_video_data(frame, &vf.frame, size);

	mVideoQueue->push_back(vf);
	mVideoQueueFrames++;
}

void AJAOutput::ClearVideoQueue()
{
	const std::lock_guard<std::mutex> lock(mVideoLock);
	while (mVideoQueue->size() > 0) {
		auto &vf = mVideoQueue->front();
		free_video_frame(&vf.frame);
		mVideoQueue->pop_front();
	}
}

size_t AJAOutput::VideoQueueSize()
{
	return mVideoQueue->size();
}

// lock video queue before calling
void AJAOutput::DMAVideoFromQueue()
{
	auto &vf = mVideoQueue->front();
	auto data = vf.frame.data[0];

	// find the next buffer
	uint32_t writeCardFrame = mWriteCardFrame + 1;
	if (writeCardFrame > mLastCardFrame)
		writeCardFrame = mFirstCardFrame;

	// use the next buffer if available
	if (writeCardFrame != mPlayCardFrame)
		mWriteCardFrame = writeCardFrame;

	mVideoWriteFrames++;

	auto result = mCard->DMAWriteFrame(mWriteCardFrame,
					   reinterpret_cast<ULWord *>(data),
					   (ULWord)vf.size);
	if (!result)
		blog(LOG_DEBUG,
		     "AJAOutput::DMAVideoFromQueue: Failed ot write video frame!");

	free_video_frame(&vf.frame);
	mVideoQueue->pop_front();
}

// TODO(paulh): Keep track of framebuffer indices used on the card, between the capture
// and output plugins, so that we can optimize frame index placement in memory and
// reduce unused gaps in between channel frame indices.
void AJAOutput::calculate_card_frame_indices(uint32_t numFrames,
					     NTV2DeviceID id,
					     NTV2Channel channel,
					     NTV2VideoFormat vf,
					     NTV2PixelFormat pf)
{
	ULWord channelIndex = GetIndexForNTV2Channel(channel);
	ULWord totalCardFrames = NTV2DeviceGetNumberFrameBuffers(
		id, GetNTV2FrameGeometryFromVideoFormat(vf), pf);
	mFirstCardFrame = channelIndex * numFrames;
	uint32_t lastFrame = mFirstCardFrame + (numFrames - 1);
	if (totalCardFrames - mFirstCardFrame > 0 &&
	    totalCardFrames - lastFrame > 0) {
		// Reserve N framebuffers in card DRAM.
		mNumCardFrames = numFrames;
		mWriteCardFrame = mFirstCardFrame;
		mLastCardFrame = lastFrame;
	} else {
		// otherwise just grab 2 frames to ping-pong between
		mNumCardFrames = 2;
		mWriteCardFrame = channelIndex * 2;
		mLastCardFrame = mWriteCardFrame + (mNumCardFrames - 1);
	}
	blog(LOG_DEBUG, "AJA Output using %d card frame indices (%d-%d)",
	     mNumCardFrames, mFirstCardFrame, mLastCardFrame);
}

uint32_t AJAOutput::get_frame_count()
{
	uint32_t frameCount = 0;
	NTV2Channel channel = mOutputProps.Channel();
	INTERRUPT_ENUMS interrupt = NTV2ChannelToOutputInterrupt(channel);
	bool isProgressiveTransport = NTV2_IS_PROGRESSIVE_STANDARD(
		::GetNTV2StandardFromVideoFormat(mOutputProps.videoFormat));

	if (isProgressiveTransport) {
		mCard->GetInterruptCount(interrupt, frameCount);
	} else {
		uint32_t intCount;
		uint32_t nextCount;
		NTV2FieldID fieldID;
		mCard->GetInterruptCount(interrupt, intCount);
		mCard->GetOutputFieldID(channel, fieldID);
		mCard->GetInterruptCount(interrupt, nextCount);
		if (intCount != nextCount) {
			mCard->GetInterruptCount(interrupt, intCount);
			mCard->GetOutputFieldID(channel, fieldID);
		}
		if (fieldID == NTV2_FIELD1)
			intCount--;
		frameCount = intCount / 2;
	}

	return frameCount;
}

void AJAOutput::CreateThread(bool enable)
{
	const std::lock_guard<std::mutex> lock(mRunThreadLock);
	if (!mRunThread.Active()) {
		mRunThread.SetPriority(AJA_ThreadPriority_High);
		mRunThread.SetThreadName("AJA Video Output Thread");
		mRunThread.Attach(AJAOutput::OutputThread, this);
	}
	if (enable) {
		mIsRunning = true;
		mRunThread.Start();
	}
}

void AJAOutput::StopThread()
{
	const std::lock_guard<std::mutex> lock(mRunThreadLock);
	mIsRunning = false;
	if (mRunThread.Active()) {
		mRunThread.Stop();
	}
}

bool AJAOutput::ThreadRunning()
{
	return mIsRunning;
}

void AJAOutput::OutputThread(AJAThread *thread, void *ctx)
{
	UNUSED_PARAMETER(thread);

	AJAOutput *ajaOutput = static_cast<AJAOutput *>(ctx);
	if (!ajaOutput) {
		blog(LOG_ERROR,
		     "AJAOutput::OutputThread: AJA Output instance is null!");
		return;
	}

	CNTV2Card *card = ajaOutput->GetCard();
	if (!card) {
		blog(LOG_ERROR,
		     "AJAOutput::OutputThread: Card instance is null!");
		return;
	}

	uint64_t videoPlayLast = ajaOutput->get_frame_count();

	// thread loop
	while (ajaOutput->ThreadRunning()) {
		// Check if a vsync occurred
		uint32_t frameCount = ajaOutput->get_frame_count();
		if (frameCount > videoPlayLast) {
			videoPlayLast = frameCount;
			ajaOutput->mPlayCardFrame = ajaOutput->mPlayCardNext;

			if (ajaOutput->mPlayCardFrame !=
			    ajaOutput->mWriteCardFrame) {
				uint32_t playCardNext =
					ajaOutput->mPlayCardFrame + 1;
				if (playCardNext > ajaOutput->mLastCardFrame)
					playCardNext =
						ajaOutput->mFirstCardFrame;

				if (playCardNext !=
				    ajaOutput->mWriteCardFrame) {
					ajaOutput->mPlayCardNext = playCardNext;
					// Increment the play frame
					ajaOutput->mCard->SetOutputFrame(
						ajaOutput->mOutputProps
							.Channel(),
						ajaOutput->mPlayCardNext);
				}
				ajaOutput->mVideoPlayFrames++;
			}
		}

		// Video DMA
		{
			const std::lock_guard<std::mutex> lock(
				ajaOutput->mVideoLock);
			while (ajaOutput->VideoQueueSize() > 0) {
				ajaOutput->DMAVideoFromQueue();
			}
		}

		os_sleep_ms(1);
	}

	blog(LOG_INFO,
	     "AJAOutput::OutputThread: Thread stopped. Played %" PRIu64 " video frames, received %" PRIu64 " video frames",
	     (uint64_t)ajaOutput->mVideoWriteFrames,
	     (uint64_t)ajaOutput->mVideoQueueFrames);
}

void populate_output_device_list(obs_property_t *list)
{
	obs_property_list_clear(list);

	auto &cardManager = aja::CardManager::Instance();
	cardManager.EnumerateCards();
	for (auto &iter : cardManager.GetCardEntries()) {
		if (!iter.second)
			continue;

		CNTV2Card *card = iter.second->GetCard();
		if (!card)
			continue;

		NTV2DeviceID deviceID = card->GetDeviceID();

		//TODO(paulh): Add support for analog I/O
		// w/ NTV2DeviceGetNumAnalogVideoOutputs(cardEntry.deviceID)
		if (NTV2DeviceGetNumVideoOutputs(deviceID) > 0 ||
		    NTV2DeviceGetNumHDMIVideoOutputs(deviceID) > 0) {

			obs_property_list_add_string(
				list, iter.second->GetDisplayName().c_str(),
				iter.second->GetCardID().c_str());
		}
	}
}

bool aja_output_device_changed(void *data, obs_properties_t *props,
			       obs_property_t *list, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);

	blog(LOG_DEBUG, "AJA Output Device Changed");

	populate_output_device_list(list);

	const char *cardID = obs_data_get_string(settings, kUIPropDevice.id);
	if (!cardID || !cardID[0])
		return false;

	const char *outputID =
		obs_data_get_string(settings, kUIPropAJAOutputID.id);
	auto &cardManager = aja::CardManager::Instance();
	cardManager.EnumerateCards();
	auto cardEntry = cardManager.GetCardEntry(cardID);
	if (!cardEntry) {
		blog(LOG_ERROR,
		     "aja_output_device_changed: Card Entry not found for %s",
		     cardID);
		return false;
	}

	CNTV2Card *card = cardEntry->GetCard();
	if (!card) {
		blog(LOG_ERROR,
		     "aja_output_device_changed: Card instance is null!");
		return false;
	}

	obs_property_t *io_select_list =
		obs_properties_get(props, kUIPropOutput.id);
	obs_property_t *vid_fmt_list =
		obs_properties_get(props, kUIPropVideoFormatSelect.id);
	obs_property_t *pix_fmt_list =
		obs_properties_get(props, kUIPropPixelFormatSelect.id);

	const NTV2DeviceID deviceID = cardEntry->GetDeviceID();
	populate_io_selection_output_list(cardID, outputID, deviceID,
					  io_select_list);

	// If Channel 1 is actively in use, filter the video format list to only
	// show video formats within the same framerate family. If Channel 1 is
	// not active we just go ahead and try to set all framestores to the same video format.
	// This is because Channel 1's clock rate will govern the card's Free Run clock.
	NTV2VideoFormat videoFormatChannel1 = NTV2_FORMAT_UNKNOWN;
	if (!cardEntry->ChannelReady(NTV2_CHANNEL1, outputID)) {
		card->GetVideoFormat(videoFormatChannel1, NTV2_CHANNEL1);
	}

	obs_property_list_clear(vid_fmt_list);
	populate_video_format_list(deviceID, vid_fmt_list, videoFormatChannel1,
				   false);

	obs_property_list_clear(pix_fmt_list);
	populate_pixel_format_list(deviceID, pix_fmt_list);

	IOSelection io_select = static_cast<IOSelection>(
		obs_data_get_int(settings, kUIPropOutput.id));

	update_sdi_transport_and_sdi_transport_4k(
		props, cardEntry->GetDeviceID(), io_select,
		static_cast<NTV2VideoFormat>(obs_data_get_int(
			settings, kUIPropVideoFormatSelect.id)));

	return true;
}

bool aja_output_dest_changed(obs_properties_t *props, obs_property_t *list,
			     obs_data_t *settings)
{
	UNUSED_PARAMETER(props);

	blog(LOG_DEBUG, "AJA Output Dest Changed");

	const char *cardID = obs_data_get_string(settings, kUIPropDevice.id);
	if (!cardID || !cardID[0])
		return false;

	auto &cardManager = aja::CardManager::Instance();
	auto cardEntry = cardManager.GetCardEntry(cardID);
	if (!cardEntry) {
		blog(LOG_DEBUG,
		     "aja_output_dest_changed: Card Entry not found for %s",
		     cardID);
		return false;
	}

	bool itemFound = false;
	const long long dest = obs_data_get_int(settings, kUIPropOutput.id);
	const size_t itemCount = obs_property_list_item_count(list);
	for (size_t i = 0; i < itemCount; i++) {
		const long long itemDest = obs_property_list_item_int(list, i);
		if (dest == itemDest) {
			itemFound = true;
			break;
		}
	}
	if (!itemFound) {
		obs_property_list_insert_int(list, 0, "", dest);
		obs_property_list_item_disable(list, 0, true);
		return true;
	}

	// Revert to "Select..." if desired IOSelection is already in use
	auto io_select = static_cast<IOSelection>(
		obs_data_get_int(settings, kUIPropOutput.id));
	for (size_t i = 0; i < obs_property_list_item_count(list); i++) {
		auto io_item = static_cast<IOSelection>(
			obs_property_list_item_int(list, i));
		if (io_item == io_select &&
		    obs_property_list_item_disabled(list, i)) {
			obs_data_set_int(
				settings, kUIPropOutput.id,
				static_cast<long long>(IOSelection::Invalid));
			blog(LOG_WARNING,
			     "aja_output_dest_changed: IOSelection %s is already in use",
			     aja::IOSelectionToString(io_select).c_str());
			return false;
		}
	}

	update_sdi_transport_and_sdi_transport_4k(
		props, cardEntry->GetDeviceID(), io_select,
		static_cast<NTV2VideoFormat>(obs_data_get_int(
			settings, kUIPropVideoFormatSelect.id)));

	return true;
}

static void aja_output_destroy(void *data)
{
	blog(LOG_DEBUG, "AJA Output Destroy");

	auto ajaOutput = (AJAOutput *)data;
	if (!ajaOutput) {
		blog(LOG_ERROR, "aja_output_destroy: Plugin instance is null!");
		return;
	}

	ajaOutput->StopThread();
	ajaOutput->ClearVideoQueue();
	delete ajaOutput;
	ajaOutput = nullptr;
}

static void *aja_output_create(obs_data_t *settings, obs_output_t *output)
{
	blog(LOG_INFO, "Creating AJA Output...");

	const char *cardID = obs_data_get_string(settings, kUIPropDevice.id);
	if (!cardID || !cardID[0])
		return nullptr;

	const char *outputID =
		obs_data_get_string(settings, kUIPropAJAOutputID.id);

	auto &cardManager = aja::CardManager::Instance();
	auto cardEntry = cardManager.GetCardEntry(cardID);
	if (!cardEntry) {
		blog(LOG_ERROR,
		     "aja_output_create: Card Entry not found for %s", cardID);
		return nullptr;
	}

	CNTV2Card *card = cardEntry->GetCard();
	if (!card) {
		blog(LOG_ERROR,
		     "aja_output_create: Card instance is null for %s", cardID);
		return nullptr;
	}

	NTV2DeviceID deviceID = card->GetDeviceID();
	OutputProps outputProps(deviceID);
	outputProps.ioSelect = static_cast<IOSelection>(
		obs_data_get_int(settings, kUIPropOutput.id));
	outputProps.videoFormat = static_cast<NTV2VideoFormat>(
		obs_data_get_int(settings, kUIPropVideoFormatSelect.id));
	outputProps.pixelFormat = static_cast<NTV2PixelFormat>(
		obs_data_get_int(settings, kUIPropPixelFormatSelect.id));
	outputProps.sdiTransport = static_cast<SDITransport>(
		obs_data_get_int(settings, kUIPropSDITransport.id));
	outputProps.sdi4kTransport = static_cast<SDITransport4K>(
		obs_data_get_int(settings, kUIPropSDITransport4K.id));
	outputProps.audioNumChannels = kDefaultAudioChannels;
	outputProps.audioSampleSize = kDefaultAudioSampleSize;
	outputProps.audioSampleRate = kDefaultAudioSampleRate;

	if (outputProps.ioSelect == IOSelection::Invalid) {
		blog(LOG_DEBUG,
		     "aja_output_create: Select a valid AJA Output IOSelection!");
		return nullptr;
	}
	if (outputProps.videoFormat == NTV2_FORMAT_UNKNOWN ||
	    outputProps.pixelFormat == NTV2_FBF_INVALID) {
		blog(LOG_ERROR,
		     "aja_output_create: Select a valid video and/or pixel format!");
		return nullptr;
	}

	const std::string &ioSelectStr =
		aja::IOSelectionToString(outputProps.ioSelect);

	NTV2OutputDestinations outputDests;
	aja::IOSelectionToOutputDests(outputProps.ioSelect, outputDests);
	if (outputDests.empty()) {
		blog(LOG_ERROR,
		     "No Output Destinations found for IOSelection %s!",
		     ioSelectStr.c_str());
		return nullptr;
	}
	outputProps.outputDest = *outputDests.begin();

	if (!cardEntry->AcquireOutputSelection(outputProps.ioSelect, deviceID,
					       outputID)) {
		blog(LOG_ERROR,
		     "aja_output_create: Error acquiring IOSelection %s for card ID %s",
		     ioSelectStr.c_str(), cardID);
		return nullptr;
	}

	auto ajaOutput = new AJAOutput(card, cardID, outputID,
				       (UWord)cardEntry->GetCardIndex(),
				       deviceID);
	ajaOutput->Initialize(outputProps);
	ajaOutput->ClearVideoQueue();
	ajaOutput->SetOBSOutput(output);
	ajaOutput->CreateThread(true);

	blog(LOG_INFO, "AJA Output created!");

	return ajaOutput;
}

static void aja_output_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(settings);
	blog(LOG_INFO, "AJA Output Update...");
}

static bool aja_output_start(void *data)
{
	blog(LOG_INFO, "Starting AJA Output...");

	auto ajaOutput = (AJAOutput *)data;
	if (!ajaOutput) {
		blog(LOG_ERROR, "aja_output_start: Plugin instance is null!");
		return false;
	}

	const std::string &cardID = ajaOutput->mCardID;
	auto &cardManager = aja::CardManager::Instance();
	cardManager.EnumerateCards();
	auto cardEntry = cardManager.GetCardEntry(cardID);
	if (!cardEntry) {
		blog(LOG_DEBUG,
		     "aja_io_selection_changed: Card Entry not found for %s",
		     cardID.c_str());
		return false;
	}

	CNTV2Card *card = ajaOutput->GetCard();
	if (!card) {
		blog(LOG_ERROR, "aja_output_start: Card instance is null!");
		return false;
	}

	auto outputProps = ajaOutput->GetOutputProps();
	auto audioSystem = outputProps.AudioSystem();
	auto outputDest = outputProps.outputDest;
	auto videoFormat = outputProps.videoFormat;
	auto pixelFormat = outputProps.pixelFormat;

	blog(LOG_INFO,
	     "Output Dest: %s | Audio System: %s | Video Format: %s | Pixel Format: %s",
	     NTV2OutputDestinationToString(outputDest, true).c_str(),
	     NTV2AudioSystemToString(audioSystem, true).c_str(),
	     NTV2VideoFormatToString(videoFormat, false).c_str(),
	     NTV2FrameBufferFormatToString(pixelFormat, true).c_str());

	const NTV2DeviceID deviceID = card->GetDeviceID();

	if (GetIndexForNTV2Channel(outputProps.Channel()) > 0) {
		auto numFramestores = aja::CardNumFramestores(deviceID);
		for (UWord i = 0; i < numFramestores; i++) {
			auto channel = GetNTV2ChannelForIndex(i);
			if (cardEntry->ChannelReady(channel,
						    ajaOutput->mOutputID)) {
				card->SetVideoFormat(videoFormat, false, false,
						     channel);
				card->SetRegisterWriteMode(
					NTV2_REGWRITE_SYNCTOFRAME, channel);
				card->SetFrameBufferFormat(channel,
							   pixelFormat);
			}
		}
	}

	// Configures crosspoint routing on AJA card
	ajaOutput->ClearConnections();
	NTV2XptConnections xpt_cnx;
	if (!aja::Routing::ConfigureOutputRoute(outputProps, NTV2_MODE_DISPLAY,
						card, xpt_cnx)) {
		blog(LOG_ERROR,
		     "aja_output_start: Error configuring output route!");
		return false;
	}
	ajaOutput->CacheConnections(xpt_cnx);
	aja::Routing::ConfigureOutputAudio(outputProps, card);

	const auto &formatDesc = outputProps.FormatDesc();
	struct video_scale_info scaler = {};
	scaler.format = aja::AJAPixelFormatToOBSVideoFormat(pixelFormat);
	scaler.width = formatDesc.GetRasterWidth();
	scaler.height = formatDesc.GetRasterHeight();
	// TODO(paulh): Find out what these scaler params actually do.
	// The colors are off when outputting the frames that OBS sends us.
	// but simply changing these values doesn't seem to have any effect.
	scaler.colorspace = VIDEO_CS_709;
	scaler.range = VIDEO_RANGE_PARTIAL;

	obs_output_set_video_conversion(ajaOutput->GetOBSOutput(), &scaler);

	struct audio_convert_info conversion = {};
	conversion.format = outputProps.AudioFormat();
	conversion.speakers = outputProps.SpeakerLayout();
	conversion.samples_per_sec = outputProps.audioSampleRate;

	obs_output_set_audio_conversion(ajaOutput->GetOBSOutput(), &conversion);

	if (!obs_output_begin_data_capture(ajaOutput->GetOBSOutput(), 0)) {
		blog(LOG_ERROR,
		     "aja_output_start: Begin OBS data capture failed!");
		return false;
	}

	blog(LOG_INFO, "AJA Output started!");

	return true;
}

static void aja_output_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);

	blog(LOG_INFO, "Stopping AJA Output...");

	auto ajaOutput = (AJAOutput *)data;
	if (!ajaOutput) {
		blog(LOG_ERROR, "aja_output_stop: Plugin instance is null!");
		return;
	}
	const std::string &cardID = ajaOutput->mCardID;
	auto &cardManager = aja::CardManager::Instance();
	cardManager.EnumerateCards();
	auto cardEntry = cardManager.GetCardEntry(cardID);
	if (!cardEntry) {
		blog(LOG_ERROR, "aja_output_stop: Card Entry not found for %s",
		     cardID.c_str());
		return;
	}
	CNTV2Card *card = ajaOutput->GetCard();
	if (!card) {
		blog(LOG_ERROR, "aja_output_stop: Card instance is null!");
		return;
	}

	auto outputProps = ajaOutput->GetOutputProps();
	if (!cardEntry->ReleaseOutputSelection(outputProps.ioSelect,
					       card->GetDeviceID(),
					       ajaOutput->mOutputID)) {
		blog(LOG_WARNING,
		     "aja_output_stop: Error releasing IOSelection %s from card ID %s",
		     aja::IOSelectionToString(outputProps.ioSelect).c_str(),
		     cardID.c_str());
	}

	ajaOutput->GenerateTestPattern(outputProps.videoFormat,
				       outputProps.pixelFormat,
				       NTV2_TestPatt_Black);

	obs_output_end_data_capture(ajaOutput->GetOBSOutput());
	ajaOutput->ClearConnections();

	blog(LOG_INFO, "AJA Output stopped.");
}

static void aja_output_raw_video(void *data, struct video_data *frame)
{
	auto ajaOutput = (AJAOutput *)data;
	if (!ajaOutput)
		return;

	auto outputProps = ajaOutput->GetOutputProps();
	auto rasterBytes = outputProps.FormatDesc().GetTotalRasterBytes();
	ajaOutput->QueueVideoFrame(frame, rasterBytes);
}

static void aja_output_raw_audio(void *data, struct audio_data *frames)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(frames);
}

static obs_properties_t *aja_output_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *device_list = obs_properties_add_list(
		props, kUIPropDevice.id, obs_module_text(kUIPropDevice.text),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_t *output_list = obs_properties_add_list(
		props, kUIPropOutput.id, obs_module_text(kUIPropOutput.text),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_t *vid_fmt_list = obs_properties_add_list(
		props, kUIPropVideoFormatSelect.id,
		obs_module_text(kUIPropVideoFormatSelect.text),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_properties_add_list(props, kUIPropPixelFormatSelect.id,
				obs_module_text(kUIPropPixelFormatSelect.text),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_properties_add_list(props, kUIPropSDITransport.id,
				obs_module_text(kUIPropSDITransport.text),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_properties_add_list(props, kUIPropSDITransport.id,
				obs_module_text(kUIPropSDITransport.text),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_properties_add_list(props, kUIPropSDITransport4K.id,
				obs_module_text(kUIPropSDITransport4K.text),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_properties_add_bool(props, kUIPropAutoStartOutput.id,
				obs_module_text(kUIPropAutoStartOutput.text));

	obs_property_set_modified_callback(vid_fmt_list,
					   aja_video_format_changed);
	obs_property_set_modified_callback(output_list,
					   aja_output_dest_changed);
	obs_property_set_modified_callback2(device_list,
					    aja_output_device_changed, data);
	return props;
}

static const char *aja_output_get_name(void *)
{
	return obs_module_text(kUIPropOutputModule.text);
}

static void aja_output_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, kUIPropOutput.id,
				 static_cast<long long>(IOSelection::Invalid));
	obs_data_set_default_int(
		settings, kUIPropVideoFormatSelect.id,
		static_cast<long long>(kDefaultAJAVideoFormat));
	obs_data_set_default_int(
		settings, kUIPropPixelFormatSelect.id,
		static_cast<long long>(kDefaultAJAPixelFormat));
	obs_data_set_default_int(
		settings, kUIPropSDITransport.id,
		static_cast<long long>(kDefaultAJASDITransport));
	obs_data_set_default_int(
		settings, kUIPropSDITransport4K.id,
		static_cast<long long>(kDefaultAJASDITransport4K));
}

struct obs_output_info create_aja_output_info()
{
	struct obs_output_info aja_output_info = {};

	aja_output_info.id = kUIPropOutputModule.id;
	aja_output_info.flags = OBS_OUTPUT_AV;
	aja_output_info.get_name = aja_output_get_name;
	aja_output_info.create = aja_output_create;
	aja_output_info.destroy = aja_output_destroy;
	aja_output_info.start = aja_output_start;
	aja_output_info.stop = aja_output_stop;
	aja_output_info.raw_video = aja_output_raw_video;
	aja_output_info.raw_audio = aja_output_raw_audio;
	aja_output_info.update = aja_output_update;
	aja_output_info.get_defaults = aja_output_defaults;
	aja_output_info.get_properties = aja_output_get_properties;
	return aja_output_info;
}
