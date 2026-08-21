/*
 * AudioFileProcessor.cpp - instrument for using audio files
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Modifications have been made to this file on August 18th, 2026 by 007CosmicClover.
 *
 */

#include "AudioFileProcessor.h"
#include "AudioFileProcessorView.h"

#include "InstrumentTrack.h"
#include "PathUtil.h"
#include "Song.h"

#include "LmmsTypes.h"
#include "plugin_export.h"

#include "MainWindow.h"
#include "GuiApplication.h"

#include <QApplication>

#include <QDomElement>
#include <iostream>
#include <fstream>
#include <filesystem>

namespace lmms
{

extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT audiofileprocessor_plugin_descriptor =
{
	LMMS_STRINGIFY( PLUGIN_NAME ),
	"AudioFileProcessor",
	QT_TRANSLATE_NOOP( "PluginBrowser",
				"Simple sampler with various settings for "
				"using samples (e.g. drums) in an "
				"instrument-track" ),
	"Tobias Doerffel <tobydox/at/users.sf.net>",
	0x0100,
	Plugin::Type::Instrument,
	new PluginPixmapLoader( "logo" ),
	"wav,ogg,ds,spx,au,voc,aif,aiff,flac,raw"
#ifdef LMMS_HAVE_SNDFILE_MP3
	",mp3"
#endif
	,
	nullptr,
} ;

}




AudioFileProcessor::AudioFileProcessor( InstrumentTrack * _instrument_track ) :
	Instrument( _instrument_track, &audiofileprocessor_plugin_descriptor ),
	m_ampModel( 100, 0, 500, 1, this, tr( "Amplify" ) ),
	m_startPointModel( 0, 0, 1, 0.0000001f, this, tr( "Start of sample" ) ),
	m_endPointModel( 1, 0, 1, 0.0000001f, this, tr( "End of sample" ) ),
	m_loopPointModel( 0, 0, 1, 0.0000001f, this, tr( "Loopback point" ) ),
	m_reverseModel( false, this, tr( "Reverse sample" ) ),
	m_loopModel( 0, 0, 2, this, tr( "Loop mode" ) ),
	m_stutterModel( false, this, tr( "Stutter" ) ),
	m_interpolationModel( this, tr( "Interpolation mode" ) ),
	m_nextPlayStartPoint( 0 ),
	m_nextPlayBackwards( false )
{
	connect( &m_reverseModel, SIGNAL( dataChanged() ),
				this, SLOT( reverseModelChanged() ), Qt::DirectConnection );
	connect( &m_ampModel, SIGNAL( dataChanged() ),
				this, SLOT( ampModelChanged() ), Qt::DirectConnection );
	connect( &m_startPointModel, SIGNAL( dataChanged() ),
				this, SLOT( startPointChanged() ), Qt::DirectConnection );
	connect( &m_endPointModel, SIGNAL( dataChanged() ),
				this, SLOT( endPointChanged() ), Qt::DirectConnection );
	connect( &m_loopPointModel, SIGNAL( dataChanged() ),
				this, SLOT( loopPointChanged() ), Qt::DirectConnection );
	connect( &m_stutterModel, SIGNAL( dataChanged() ),
				this, SLOT( stutterModelChanged() ), Qt::DirectConnection );
	connect( &m_loopModel, SIGNAL( dataChanged() ),
				this, SLOT( onLoopModeChanged() ), Qt::DirectConnection);
	
	
//interpolation modes
	m_interpolationModel.addItem( tr( "None" ) );
	m_interpolationModel.addItem( tr( "Linear" ) );
	m_interpolationModel.addItem( tr( "Sinc" ) );
	m_interpolationModel.setValue( 1 );

	pointChanged();
}

void AudioFileProcessor::saveFloatArrayToWav(const std::string& filename, const float* floatData, size_t numSamples, sample_rate_t sampleRate = 44100)
{
	unsigned int counter = 1;
	std::filesystem::path path = filename;
	// Check if the file already exists; if so, alter the filename
	while (std::filesystem::exists(path)) {
		path = path.parent_path() / 
			(path.stem().string() + "_" + std::to_string(counter) + path.extension().string());
		counter++;
	}
	
	std::ofstream file(path, std::ios::binary);
	if (!file.is_open()) {
		return;
	}
		   // 1. Calculate Sizes
	int numChannels = 1; // Mono
	int bytesPerSample = 2; // 16-bit PCM = 2 bytes
	int byteRate = sampleRate * numChannels * bytesPerSample;
	int blockAlign = numChannels * bytesPerSample;
	int subChunk2Size = numSamples * numChannels * bytesPerSample;
	int chunkSize = 36 + subChunk2Size;
	
		   // 2. Write the RIFF Header
	file.write("RIFF", 4);
	file.write(reinterpret_cast<const char*>(&chunkSize), 4);
	file.write("WAVE", 4);
	
		   // 3. Write the "fmt " Sub-chunk
	file.write("fmt ", 4);
	int subChunk1Size = 16; // 16 for PCM
	file.write(reinterpret_cast<const char*>(&subChunk1Size), 4);
	
	short audioFormat = 1; // 1 = Uncompressed PCM
	file.write(reinterpret_cast<const char*>(&audioFormat), 2);
	file.write(reinterpret_cast<const char*>(&numChannels), 2);
	file.write(reinterpret_cast<const char*>(&sampleRate), 4);
	file.write(reinterpret_cast<const char*>(&byteRate), 4);
	file.write(reinterpret_cast<const char*>(&blockAlign), 2);
	
	short bitsPerSample = 16;
	file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
	
		   // 4. Write the "data" Sub-chunk descriptor
	file.write("data", 4);
	file.write(reinterpret_cast<const char*>(&subChunk2Size), 4);
	
		   // 5. Convert floats (-1.0 to 1.0) to 16-bit shorts (-32768 to 32767) and write
	for (size_t i = 0; i < numSamples; ++i) {
		// Clamp the float to prevent digital clipping/overflow distortions
		float sample = std::max(-1.0f, std::min(1.0f, floatData[i]));
		
		// Scale to 16-bit integer range
		short intSample = static_cast<short>(sample * 32767.0f);
		
		file.write(reinterpret_cast<const char*>(&intSample), sizeof(short));
	}
	
	file.close();
}

void AudioFileProcessor::onLoopModeChanged()
{
	
}

void AudioFileProcessor::fixLoop()
{
	const auto f_end = static_cast<f_cnt_t>(m_endPointModel.value() * m_sample.sampleSize());
	const auto f_loop = static_cast<f_cnt_t>(m_loopPointModel.value() * m_sample.sampleSize());
	
	//get all the frames for the sample
	const SampleFrame* sampleFrames = m_sample.data();
	//get the sample size
	unsigned long totalSampleFrames = m_sample.sampleSize();
	//get the first frame of loop
	const float* firstDataOfLoop = sampleFrames[f_loop].data();
	unsigned long newFirstFrameOfLoop = f_loop;
	
	if(*firstDataOfLoop == 0){
		for(; newFirstFrameOfLoop < totalSampleFrames;newFirstFrameOfLoop++){
			firstDataOfLoop = sampleFrames[newFirstFrameOfLoop].data();
			if(*firstDataOfLoop != 0){
				break;
			}
		}
	}
	
	unsigned long newLastFrameOfLoop = -1;
	
	if(totalSampleFrames > 0){
		if(*firstDataOfLoop < 0){
			//iterate until data is > 0
			for(;newFirstFrameOfLoop < totalSampleFrames;newFirstFrameOfLoop++){
				//get data of this frame
				const float* dataOfThisFrame = sampleFrames[newFirstFrameOfLoop].data();
				if(*dataOfThisFrame > 0){
					break;
				}
			}
		}else if(*firstDataOfLoop > 0){
			//iterate until data is < 0
			for(;newFirstFrameOfLoop < totalSampleFrames;newFirstFrameOfLoop++){
				//get data of this frame
				const float* dataOfThisFrame = sampleFrames[newFirstFrameOfLoop].data();
				if(*dataOfThisFrame < 0){
					break;
				}
			}
		}		
		//make sure it's not at zero this time
		const float* lastDataOfLoop = sampleFrames[f_end].data();
		newLastFrameOfLoop = f_end;
		if(*lastDataOfLoop == 0){
			for(; newLastFrameOfLoop < totalSampleFrames;newLastFrameOfLoop++){
				lastDataOfLoop = sampleFrames[newLastFrameOfLoop].data();
				if(*lastDataOfLoop != 0){
					break;
				}
			}
		}
		
		//get end of loop ready
		if(*firstDataOfLoop < 0 && *lastDataOfLoop > 0){
			//make end of loop in the minus
			for(;newLastFrameOfLoop < totalSampleFrames;newLastFrameOfLoop++){
				//get data of this frame
				const float* dataOfThisFrame = sampleFrames[newLastFrameOfLoop].data();
				if(*dataOfThisFrame < 0){
					break;
				}	
			}
		}
		if(*firstDataOfLoop < 0){
			//now make end of loop right before crossing 0
			//iterate until data is > 0
			for(unsigned long index = newLastFrameOfLoop;index < totalSampleFrames;index++){
				
				//get data of this frame
				const float* dataOfThisFrame = sampleFrames[index].data();
				if(*dataOfThisFrame > 0){
					newLastFrameOfLoop = index - 1; // use index before crossing 0 for loop end
					break;
				}
			}
		}
		//get end of loop ready
		if(*firstDataOfLoop > 0 && *lastDataOfLoop < 0){
			//make end of loop in the pluss
			for(;newLastFrameOfLoop < totalSampleFrames;newLastFrameOfLoop++){
				//get data of this frame
				const float* dataOfThisFrame = sampleFrames[newLastFrameOfLoop].data();
				if(*dataOfThisFrame > 0){
					break;
				}	
			}
		}
		if(*firstDataOfLoop > 0 ){
			//now make end of loop right before crossing 0
			//iterate until data is < 0
			for(unsigned long index = newLastFrameOfLoop;index < totalSampleFrames;index++){
				
				//get data of this frame
				const float* dataOfThisFrame = sampleFrames[index].data();
				if(*dataOfThisFrame < 0){
					newLastFrameOfLoop = index - 1; // use index before crossing 0 for loop end
					break;
				}
			}		
		}
		if(m_startPointModel.value() > m_loopPointModel.value() || m_startPointModel.value() >= m_endPointModel.value() ||
			m_loopPointModel.value() >= m_endPointModel.value()){//just in case
			m_startPointModel.setValue(.1f);
			m_loopPointModel.setValue(.2f);
			m_endPointModel.setValue(.9f);
		}else{
			m_loopPointModel.setValue((double)newFirstFrameOfLoop / (double)totalSampleFrames);
			m_endPointModel.setValue((double)newLastFrameOfLoop / (double)totalSampleFrames);
		}
		
	}
	
	const auto f_start2 = static_cast<f_cnt_t>(m_startPointModel.value() * m_sample.sampleSize());
	const auto f_end2 = static_cast<f_cnt_t>(m_endPointModel.value() * m_sample.sampleSize());
	const auto f_loop2 = static_cast<f_cnt_t>(m_loopPointModel.value() * m_sample.sampleSize());

	m_nextPlayStartPoint = f_start2;
	
	m_sample.setAllPointFrames(f_start2, f_end2, f_loop2, f_end2);
	emit dataChanged();
	
}

void AudioFileProcessor::saveLoop()
{
	//save loop to file
	const SampleFrame* sampleFrames = m_sample.data();
	const auto f_loop = static_cast<f_cnt_t>(m_loopPointModel.value() * m_sample.sampleSize());
	const auto f_end = static_cast<f_cnt_t>(m_endPointModel.value() * m_sample.sampleSize());
	unsigned long indexOfBuffer = 0;
	size_t bufferSize = f_end - f_loop;
	std::vector<float> audioBuffer(bufferSize, 0.0f); // Pre-fills everything with 0.0
	for(unsigned long index = f_loop; index < f_end; index++){
		const float* dataOfThisFrame = sampleFrames[index].data();			
		
		audioBuffer[indexOfBuffer++] = *dataOfThisFrame;
		
	}
	
		   // 1. Get the raw string and convert it to QString
	QString samplePath = QString::fromStdString(m_sample.sampleFile().toStdString());
	int colonIdx = samplePath.indexOf(':');
	if (colonIdx != -1) {
		samplePath = samplePath.mid(colonIdx + 1); // Grabs everything from after the colon to the end
	}
	
	QFileInfo fileInfo(samplePath);
	QString baseName = fileInfo.completeBaseName();
	QString exportFileName = baseName + "_loop.wav";
	
	sample_rate_t sampleRate = Instrument::getSampleRate();
	
	AudioFileProcessor::saveFloatArrayToWav(exportFileName.toStdString(), audioBuffer.data(), audioBuffer.size(), sampleRate);
	
}

void AudioFileProcessor::playNote( NotePlayHandle * _n,
						SampleFrame* _working_buffer )
{
	
	
	const f_cnt_t frames = _n->framesLeftForCurrentPeriod();
	const f_cnt_t offset = _n->noteOffset();

	// Magic key - a frequency < 20 (say, the bottom piano note if using
	// a A4 base tuning) restarts the start point. The note is not actually
	// played.
	if( m_stutterModel.value() == true && _n->frequency() < 20.0 )
	{
		m_nextPlayStartPoint = m_sample.startFrame();
		m_nextPlayBackwards = false;
		return;
	}

	if( !_n->m_pluginData )
	{
		if (m_stutterModel.value() == true && m_nextPlayStartPoint >= static_cast<std::size_t>(m_sample.endFrame()))
		{
			// Restart playing the note if in stutter mode, not in loop mode,
			// and we're at the end of the sample.
			m_nextPlayStartPoint = m_sample.startFrame();
			m_nextPlayBackwards = false;
		}
		// set interpolation mode for libsamplerate
		auto interpolationMode = AudioResampler::Mode::Linear;
		switch( m_interpolationModel.value() )
		{
			case 0:
				interpolationMode = AudioResampler::Mode::ZOH;
				break;
			case 1:
				interpolationMode = AudioResampler::Mode::Linear;
				break;
			case 2:
				interpolationMode = AudioResampler::Mode::SincMedium;
				break;
		}

		_n->m_pluginData = new Sample::PlaybackState(interpolationMode);
		static_cast<Sample::PlaybackState*>(_n->m_pluginData)->setFrameIndex(m_nextPlayStartPoint);
		static_cast<Sample::PlaybackState*>(_n->m_pluginData)->setBackwards(m_nextPlayBackwards);

// debug code
/*		qDebug( "frames %d", m_sample->frames() );
		qDebug( "startframe %d", m_sample->startFrame() );
		qDebug( "nextPlayStartPoint %d", m_nextPlayStartPoint );*/
	}

	if( ! _n->isFinished() )
	{
		if (m_sample.play(_working_buffer + offset,
						static_cast<Sample::PlaybackState*>(_n->m_pluginData),
						frames, static_cast<Sample::Loop>(m_loopModel.value()),
						DefaultBaseFreq / _n->frequency()))
		{
			applyRelease( _working_buffer, _n );
			emit isPlaying(static_cast<Sample::PlaybackState*>(_n->m_pluginData)->frameIndex());
		}
		else
		{
			zeroSampleFrames(_working_buffer, frames + offset);
			emit isPlaying( 0 );
		}
	}
	else
	{
		emit isPlaying( 0 );
	}
	if( m_stutterModel.value() == true )
	{
		m_nextPlayStartPoint = static_cast<Sample::PlaybackState*>(_n->m_pluginData)->frameIndex();
		m_nextPlayBackwards = static_cast<Sample::PlaybackState*>(_n->m_pluginData)->backwards();
	}
}




void AudioFileProcessor::deleteNotePluginData( NotePlayHandle * _n )
{
	delete static_cast<Sample::PlaybackState*>(_n->m_pluginData);
}




void AudioFileProcessor::saveSettings(QDomDocument& doc, QDomElement& elem)
{
	elem.setAttribute("src", m_sample.sampleFile());
	if (m_sample.sampleFile().isEmpty())
	{
		elem.setAttribute("sampledata", m_sample.toBase64());
	}
	m_reverseModel.saveSettings(doc, elem, "reversed");
	m_loopModel.saveSettings(doc, elem, "looped");
	m_ampModel.saveSettings(doc, elem, "amp");
	m_startPointModel.saveSettings(doc, elem, "sframe");
	m_endPointModel.saveSettings(doc, elem, "eframe");
	m_loopPointModel.saveSettings(doc, elem, "lframe");
	m_stutterModel.saveSettings(doc, elem, "stutter");
	m_interpolationModel.saveSettings(doc, elem, "interp");

}




void AudioFileProcessor::loadSettings(const QDomElement& elem)
{
	if (auto srcFile = elem.attribute("src"); !srcFile.isEmpty())
	{
		if (QFileInfo(PathUtil::toAbsolute(srcFile)).exists())
		{
			setAudioFile(srcFile, false);
		}
		else { Engine::getSong()->collectError(QString("%1: %2").arg(tr("Sample not found"), srcFile)); }
	}
	else if (auto sampleData = elem.attribute("sampledata"); !sampleData.isEmpty())
	{
		m_sample = Sample(SampleBuffer::fromBase64(sampleData));
	}

	m_loopModel.loadSettings(elem, "looped");
	
	
	m_ampModel.loadSettings(elem, "amp");
	m_endPointModel.loadSettings(elem, "eframe");
	m_startPointModel.loadSettings(elem, "sframe");

	// compat code for not having a separate loopback point
	if (elem.hasAttribute("lframe") || !elem.firstChildElement("lframe").isNull())
	{
		m_loopPointModel.loadSettings(elem, "lframe");
	}
	else
	{
		m_loopPointModel.loadSettings(elem, "sframe");
	}

	m_reverseModel.loadSettings(elem, "reversed");

	m_stutterModel.loadSettings(elem, "stutter");
	if (elem.hasAttribute("interp") || !elem.firstChildElement("interp").isNull())
	{
		m_interpolationModel.loadSettings(elem, "interp");
	}
	else
	{
		m_interpolationModel.setValue(1.0f); // linear by default
	}

	pointChanged();
	emit sampleUpdated();
}




void AudioFileProcessor::loadFile( const QString & _file )
{
	setAudioFile( _file );
}




QString AudioFileProcessor::nodeName() const
{
	return audiofileprocessor_plugin_descriptor.name;
}




auto AudioFileProcessor::beatLen(NotePlayHandle* note) const -> f_cnt_t
{
	// If we can play indefinitely, use the default beat note duration
	if (static_cast<Sample::Loop>(m_loopModel.value()) != Sample::Loop::Off) { return 0; }

	// Otherwise, use the remaining sample duration
	const auto baseFreq = instrumentTrack()->baseFreq();
	const auto freqFactor = baseFreq / note->frequency()
		* Engine::audioEngine()->outputSampleRate()
		/ Engine::audioEngine()->baseSampleRate();
	const auto sampleRateRatio = static_cast<double>(Engine::audioEngine()->outputSampleRate()) / m_sample.sampleRate();

	const auto startFrame = m_nextPlayStartPoint >= static_cast<std::size_t>(m_sample.endFrame())
		? m_sample.startFrame()
		: m_nextPlayStartPoint;
	const auto duration = m_sample.endFrame() - startFrame;

	return static_cast<f_cnt_t>(std::floor(duration * freqFactor * sampleRateRatio));
}




gui::PluginView* AudioFileProcessor::instantiateView( QWidget * _parent )
{
	return new gui::AudioFileProcessorView( this, _parent );
}

void AudioFileProcessor::setAudioFile(const QString& _audio_file, bool _rename)
{
	// is current channel-name equal to previous-filename??
	if( _rename &&
		( instrumentTrack()->name() ==
			QFileInfo(m_sample.sampleFile()).fileName() ||
				m_sample.sampleFile().isEmpty()))
	{
		// then set it to new one
		instrumentTrack()->setName( PathUtil::cleanName( _audio_file ) );
	}
	// else we don't touch the track-name, because the user named it self

	m_sample = Sample(SampleBuffer::fromFile(_audio_file));
	loopPointChanged();
	ampModelChanged();
	reverseModelChanged();
	emit sampleUpdated();
}




void AudioFileProcessor::reverseModelChanged()
{
	m_sample.setReversed(m_reverseModel.value());
	m_nextPlayStartPoint = m_sample.startFrame();
	m_nextPlayBackwards = false;
	emit sampleUpdated();
}




void AudioFileProcessor::ampModelChanged()
{
	m_sample.setAmplification(m_ampModel.value() / 100.0f);
	emit sampleUpdated();
}


void AudioFileProcessor::stutterModelChanged()
{
	m_nextPlayStartPoint = m_sample.startFrame();
	m_nextPlayBackwards = false;
}


void AudioFileProcessor::startPointChanged()
{
	// check if start is over end and swap values if so
	if( m_startPointModel.value() > m_endPointModel.value() )
	{
		float tmp = m_endPointModel.value();
		m_endPointModel.setValue( m_startPointModel.value() );
		m_startPointModel.setValue( tmp );
	}

	// nudge loop point with end
	if( m_loopPointModel.value() >= m_endPointModel.value() )
	{
		m_loopPointModel.setValue( qMax( m_endPointModel.value() - 0.001f, 0.0f ) );
	}

	// nudge loop point with start
	if( m_loopPointModel.value() < m_startPointModel.value() )
	{
		m_loopPointModel.setValue( m_startPointModel.value() );
	}

	// check if start & end overlap and nudge end up if so
	if( m_startPointModel.value() == m_endPointModel.value() )
	{
		m_endPointModel.setValue( qMin( m_endPointModel.value() + 0.001f, 1.0f ) );
	}

	pointChanged();

}

void AudioFileProcessor::endPointChanged()
{
	// same as start, for now
	startPointChanged();

}

void AudioFileProcessor::loopPointChanged()
{
	

	// check that loop point is between start-end points and not overlapping with endpoint
	// ...and move start/end points ahead if loop point is moved over them
	if( m_loopPointModel.value() >= m_endPointModel.value() )
	{
		m_endPointModel.setValue( m_loopPointModel.value() + 0.001f );
		if( m_endPointModel.value() == 1.0f )
		{
			m_loopPointModel.setValue( 1.0f - 0.001f );
		}
	}

	// nudge start point with loop
	if( m_loopPointModel.value() < m_startPointModel.value() )
	{
		m_startPointModel.setValue( m_loopPointModel.value() );
	}

	pointChanged();
}

void AudioFileProcessor::pointChanged()
{
	const auto f_start = static_cast<f_cnt_t>(m_startPointModel.value() * m_sample.sampleSize());
	const auto f_end = static_cast<f_cnt_t>(m_endPointModel.value() * m_sample.sampleSize());
	const auto f_loop = static_cast<f_cnt_t>(m_loopPointModel.value() * m_sample.sampleSize());
	
	m_nextPlayStartPoint = f_start;
	m_nextPlayBackwards = false;
	
	m_sample.setAllPointFrames(f_start, f_end, f_loop, f_end);
	emit dataChanged();
}


extern "C"
{

// necessary for getting instance out of shared lib
PLUGIN_EXPORT Plugin * lmms_plugin_main(Model * model, void *)
{
	return new AudioFileProcessor(static_cast<InstrumentTrack *>(model));
}


}


} // namespace lmms
