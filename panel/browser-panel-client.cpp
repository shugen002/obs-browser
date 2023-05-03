#include "browser-panel-client.hpp"
#include <util/dstr.h>

#include <QUrl>
#include <QDesktopServices>
#include <QApplication>
#include <QMenu>
#include <QThread>
#include <QMessageBox>
#include <QInputDialog>
#include <QRegularExpression>
#include <QLabel>
#include <QClipboard>

#include <obs-module.h>
#include "json11/json11.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
#include <X11/Xlib.h>
#endif
#include <obs-frontend-api.h>
#include <obs.hpp>

#define MENU_ITEM_DEVTOOLS MENU_ID_CUSTOM_FIRST
#define MENU_ITEM_MUTE MENU_ID_CUSTOM_FIRST + 1
#define MENU_ITEM_ZOOM_IN MENU_ID_CUSTOM_FIRST + 2
#define MENU_ITEM_ZOOM_RESET MENU_ID_CUSTOM_FIRST + 3
#define MENU_ITEM_ZOOM_OUT MENU_ID_CUSTOM_FIRST + 4
#define MENU_ITEM_COPY_URL MENU_ID_CUSTOM_FIRST + 5

using namespace json11;

/* CefClient */
CefRefPtr<CefLoadHandler> QCefBrowserClient::GetLoadHandler()
{
	return this;
}

CefRefPtr<CefDisplayHandler> QCefBrowserClient::GetDisplayHandler()
{
	return this;
}

CefRefPtr<CefRequestHandler> QCefBrowserClient::GetRequestHandler()
{
	return this;
}

CefRefPtr<CefLifeSpanHandler> QCefBrowserClient::GetLifeSpanHandler()
{
	return this;
}

CefRefPtr<CefFocusHandler> QCefBrowserClient::GetFocusHandler()
{
	return this;
}

CefRefPtr<CefContextMenuHandler> QCefBrowserClient::GetContextMenuHandler()
{
	return this;
}

CefRefPtr<CefKeyboardHandler> QCefBrowserClient::GetKeyboardHandler()
{
	return this;
}

CefRefPtr<CefJSDialogHandler> QCefBrowserClient::GetJSDialogHandler()
{
	return this;
}

/* CefDisplayHandler */
void QCefBrowserClient::OnTitleChange(CefRefPtr<CefBrowser> browser,
				      const CefString &title)
{
	if (widget && widget->cefBrowser->IsSame(browser)) {
		std::string str_title = title;
		QString qt_title = QString::fromUtf8(str_title.c_str());
		QMetaObject::invokeMethod(widget, "titleChanged",
					  Q_ARG(QString, qt_title));
	} else { /* handle popup title */
		if (title.compare("DevTools") == 0)
			return;

#if defined(_WIN32)
		CefWindowHandle handl = browser->GetHost()->GetWindowHandle();
		std::wstring str_title = title;
		SetWindowTextW((HWND)handl, str_title.c_str());
#elif defined(__linux__)
		CefWindowHandle handl = browser->GetHost()->GetWindowHandle();
		XStoreName(cef_get_xdisplay(), handl, title.ToString().c_str());
#endif
	}
}

/* CefRequestHandler */
bool QCefBrowserClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
				       CefRefPtr<CefFrame>,
				       CefRefPtr<CefRequest> request, bool,
				       bool)
{
	std::string str_url = request->GetURL();

	std::lock_guard<std::mutex> lock(popup_whitelist_mutex);
	for (size_t i = forced_popups.size(); i > 0; i--) {
		PopupWhitelistInfo &info = forced_popups[i - 1];

		if (!info.obj) {
			forced_popups.erase(forced_popups.begin() + (i - 1));
			continue;
		}

		if (astrcmpi(info.url.c_str(), str_url.c_str()) == 0) {
			/* Open tab popup URLs in user's actual browser */
			QUrl url = QUrl(str_url.c_str(), QUrl::TolerantMode);
			QDesktopServices::openUrl(url);
			browser->GoBack();
			return true;
		}
	}

	if (widget) {
		QString qt_url = QString::fromUtf8(str_url.c_str());
		QMetaObject::invokeMethod(widget, "urlChanged",
					  Q_ARG(QString, qt_url));
	}
	return false;
}

bool QCefBrowserClient::OnOpenURLFromTab(
	CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString &target_url,
	CefRequestHandler::WindowOpenDisposition, bool)
{
	std::string str_url = target_url;

	/* Open tab popup URLs in user's actual browser */
	QUrl url = QUrl(str_url.c_str(), QUrl::TolerantMode);
	QDesktopServices::openUrl(url);
	return true;
}

void QCefBrowserClient::OnLoadError(CefRefPtr<CefBrowser> browser,
				    CefRefPtr<CefFrame> frame,
				    CefLoadHandler::ErrorCode errorCode,
				    const CefString &errorText,
				    const CefString &failedUrl)
{
	UNUSED_PARAMETER(browser);
	if (errorCode == ERR_ABORTED)
		return;

	struct dstr html;
	char *path = obs_module_file("error.html");
	char *errorPage = os_quick_read_utf8_file(path);

	dstr_init_copy(&html, errorPage);

	dstr_replace(&html, "%%ERROR_URL%%", failedUrl.ToString().c_str());

	dstr_replace(&html, "Error.Title", obs_module_text("Error.Title"));
	dstr_replace(&html, "Error.Description",
		     obs_module_text("Error.Description"));
	dstr_replace(&html, "Error.Retry", obs_module_text("Error.Retry"));
	const char *translError;
	std::string errorKey = "ErrorCode." + errorText.ToString();
	if (obs_module_get_string(errorKey.c_str(),
				  (const char **)&translError)) {
		dstr_replace(&html, "%%ERROR_CODE%%", translError);
	} else {
		dstr_replace(&html, "%%ERROR_CODE%%",
			     errorText.ToString().c_str());
	}

	frame->LoadURL(
		"data:text/html;base64," +
		CefURIEncode(CefBase64Encode(html.array, html.len), false)
			.ToString());

	dstr_free(&html);
	bfree(path);
	bfree(errorPage);
}

/* CefLifeSpanHandler */
bool QCefBrowserClient::OnBeforePopup(
	CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString &target_url,
	const CefString &, CefLifeSpanHandler::WindowOpenDisposition, bool,
	const CefPopupFeatures &, CefWindowInfo &windowInfo,
	CefRefPtr<CefClient> &, CefBrowserSettings &,
	CefRefPtr<CefDictionaryValue> &, bool *)
{
	if (allowAllPopups) {
#ifdef _WIN32
		HWND hwnd = (HWND)widget->effectiveWinId();
		windowInfo.parent_window = hwnd;
#else
		UNUSED_PARAMETER(windowInfo);
#endif
		return false;
	}

	std::string str_url = target_url;

	std::lock_guard<std::mutex> lock(popup_whitelist_mutex);
	for (size_t i = popup_whitelist.size(); i > 0; i--) {
		PopupWhitelistInfo &info = popup_whitelist[i - 1];

		if (!info.obj) {
			popup_whitelist.erase(popup_whitelist.begin() +
					      (i - 1));
			continue;
		}

		if (astrcmpi(info.url.c_str(), str_url.c_str()) == 0) {
#ifdef _WIN32
			HWND hwnd = (HWND)widget->effectiveWinId();
			windowInfo.parent_window = hwnd;
#endif
			return false;
		}
	}

	/* Open popup URLs in user's actual browser */
	QUrl url = QUrl(str_url.c_str(), QUrl::TolerantMode);
	QDesktopServices::openUrl(url);
	return true;
}

bool QCefBrowserClient::OnSetFocus(CefRefPtr<CefBrowser>,
				   CefFocusHandler::FocusSource source)
{
	/* Don't steal focus when the webpage navigates. This is especially
	   obvious on startup when the user has many browser docks defined,
	   as each one will steal focus one by one, resulting in poor UX.
	 */
	switch (source) {
	case FOCUS_SOURCE_NAVIGATION:
		return true;
	default:
		return false;
	}
}

void QCefBrowserClient::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
					    CefRefPtr<CefFrame>,
					    CefRefPtr<CefContextMenuParams>,
					    CefRefPtr<CefMenuModel> model)
{
	if (model->IsVisible(MENU_ID_BACK) &&
	    (!model->IsVisible(MENU_ID_RELOAD) &&
	     !model->IsVisible(MENU_ID_RELOAD_NOCACHE))) {
		model->InsertItemAt(
			2, MENU_ID_RELOAD_NOCACHE,
			QObject::tr("RefreshBrowser").toUtf8().constData());
	}
	if (model->IsVisible(MENU_ID_PRINT)) {
		model->Remove(MENU_ID_PRINT);
	}
	if (model->IsVisible(MENU_ID_VIEW_SOURCE)) {
		model->Remove(MENU_ID_VIEW_SOURCE);
	}
	model->AddItem(MENU_ITEM_ZOOM_IN, obs_module_text("Zoom.In"));
	if (browser->GetHost()->GetZoomLevel() != 0) {
		model->AddItem(MENU_ITEM_ZOOM_RESET,
			       obs_module_text("Zoom.Reset"));
	}
	model->AddItem(MENU_ITEM_ZOOM_OUT, obs_module_text("Zoom.Out"));
	model->AddSeparator();
	model->InsertItemAt(model->GetCount(), MENU_ITEM_COPY_URL,
			    obs_module_text("CopyUrl"));
	model->InsertItemAt(model->GetCount(), MENU_ITEM_DEVTOOLS,
			    obs_module_text("Inspect"));
	model->InsertCheckItemAt(model->GetCount(), MENU_ITEM_MUTE,
				 QObject::tr("Mute").toUtf8().constData());
	model->SetChecked(MENU_ITEM_MUTE, browser->GetHost()->IsAudioMuted());
}

#if defined(_WIN32)
bool QCefBrowserClient::RunContextMenu(
	CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
	CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel> model,
	CefRefPtr<CefRunContextMenuCallback> callback)
{
	std::vector<std::tuple<std::string, int, bool, int, bool>> menu_items;
	menu_items.reserve(model->GetCount());
	for (int i = 0; i < model->GetCount(); i++) {
		menu_items.push_back(
			{model->GetLabelAt(i), model->GetCommandIdAt(i),
			 model->IsEnabledAt(i), model->GetTypeAt(i),
			 model->IsCheckedAt(i)});
	}

	QMetaObject::invokeMethod(
		QCoreApplication::instance()->thread(),
		[menu_items, callback]() {
			QMenu contextMenu;
			std::string name;
			int command_id;
			bool enabled;
			int type_id;
			bool check;

			for (const std::tuple<std::string, int, bool, int, bool>
				     &menu_item : menu_items) {
				std::tie(name, command_id, enabled, type_id,
					 check) = menu_item;
				switch (type_id) {
				case MENUITEMTYPE_CHECK:
				case MENUITEMTYPE_COMMAND: {
					QAction *item =
						new QAction(name.c_str());
					item->setEnabled(enabled);
					if (type_id == MENUITEMTYPE_CHECK) {
						item->setCheckable(true);
						item->setChecked(check);
					}
					item->setProperty("cmd_id", command_id);
					contextMenu.addAction(item);
				} break;
				case MENUITEMTYPE_SEPARATOR:
					contextMenu.addSeparator();
					break;
				}
			}

			QAction *action = contextMenu.exec(QCursor::pos());
			if (action) {
				QVariant cmdId = action->property("cmd_id");
				callback.get()->Continue(cmdId.toInt(),
							 EVENTFLAG_NONE);
			} else {
				callback.get()->Cancel();
			}
		});
	return true;
}
#endif

bool QCefBrowserClient::OnContextMenuCommand(
	CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
	CefRefPtr<CefContextMenuParams> params, int command_id,
	CefContextMenuHandler::EventFlags)
{
	if (command_id < MENU_ID_CUSTOM_FIRST)
		return false;
	CefRefPtr<CefBrowserHost> host = browser->GetHost();
	CefWindowInfo windowInfo;
	QPoint pos;
	QString title;
	switch (command_id) {
	case MENU_ITEM_DEVTOOLS:
#if defined(_WIN32)
		title = QString(obs_module_text("DevTools"))
				.arg(widget->parentWidget()->windowTitle());
		windowInfo.SetAsPopup(host->GetWindowHandle(),
				      title.toUtf8().constData());
#endif
		pos = widget->mapToGlobal(QPoint(0, 0));
		windowInfo.bounds.x = pos.x();
		windowInfo.bounds.y = pos.y() + 30;
		windowInfo.bounds.width = 900;
		windowInfo.bounds.height = 700;
		host->ShowDevTools(
			windowInfo, host->GetClient(), CefBrowserSettings(),
			{params.get()->GetXCoord(), params.get()->GetYCoord()});
		return true;
	case MENU_ITEM_MUTE:
		host->SetAudioMuted(!host->IsAudioMuted());
		return true;
	case MENU_ITEM_ZOOM_IN:
		widget->zoomPage(1);
		return true;
	case MENU_ITEM_ZOOM_RESET:
		widget->zoomPage(0);
		return true;
	case MENU_ITEM_ZOOM_OUT:
		widget->zoomPage(-1);
		return true;
	case MENU_ITEM_COPY_URL:
		std::string url = browser->GetMainFrame()->GetURL().ToString();
		auto saveClipboard = [url]() {
			QClipboard *clipboard = QApplication::clipboard();

			clipboard->setText(url.c_str(), QClipboard::Clipboard);

			if (clipboard->supportsSelection()) {
				clipboard->setText(url.c_str(),
						   QClipboard::Selection);
			}
		};
		QMetaObject::invokeMethod(
			QCoreApplication::instance()->thread(), saveClipboard);
		return true;
		break;
	}
	return false;
}

void QCefBrowserClient::OnLoadEnd(CefRefPtr<CefBrowser>,
				  CefRefPtr<CefFrame> frame, int)
{
	if (frame->IsMain() && !script.empty())
		frame->ExecuteJavaScript(script, CefString(), 0);
}

bool QCefBrowserClient::OnJSDialog(CefRefPtr<CefBrowser>, const CefString &,
				   CefJSDialogHandler::JSDialogType dialog_type,
				   const CefString &message_text,
				   const CefString &default_prompt_text,
				   CefRefPtr<CefJSDialogCallback> callback,
				   bool &)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
	QString parentTitle = widget->parentWidget()->windowTitle();
	std::string default_value = default_prompt_text;
	QString msg_raw(message_text.ToString().c_str());
	// Replace <br> with standard newline as we will render in plaintext
	msg_raw.replace(QRegularExpression("<br\\s{0,1}\\/{0,1}>"), "\n");
	QString submsg =
		QString(obs_module_text("Dialog.ReceivedFrom")).arg(parentTitle);
	QString msg = QString("%1\n\n\n%2").arg(msg_raw).arg(submsg);

	if (dialog_type == JSDIALOGTYPE_PROMPT) {
		auto msgbox = [msg, default_value, callback]() {
			QInputDialog *dlg = new QInputDialog(nullptr);
			dlg->setWindowFlag(Qt::WindowStaysOnTopHint, true);
			dlg->setWindowFlag(Qt::WindowContextHelpButtonHint,
					   false);
			std::stringstream title;
			title << obs_module_text("Dialog.Prompt") << ": "
			      << obs_module_text("Dialog.BrowserDock");
			dlg->setWindowTitle(title.str().c_str());
			if (!default_value.empty())
				dlg->setTextValue(default_value.c_str());

			auto finished = [callback, dlg](int result) {
				callback.get()->Continue(
					result == QDialog::Accepted,
					dlg->textValue().toUtf8().constData());
			};

			QWidget::connect(dlg, &QInputDialog::finished,
					 finished);
			dlg->open();
			if (QLabel *lbl = dlg->findChild<QLabel *>()) {
				// Force plaintext manually
				lbl->setTextFormat(Qt::PlainText);
			}
			dlg->setLabelText(msg);
		};
		QMetaObject::invokeMethod(
			QCoreApplication::instance()->thread(), msgbox);
		return true;
	}
	auto msgbox = [msg, dialog_type, callback]() {
		QMessageBox *dlg = new QMessageBox(nullptr);
		dlg->setStandardButtons(QMessageBox::Ok);
		dlg->setWindowFlag(Qt::WindowStaysOnTopHint, true);
		dlg->setTextFormat(Qt::PlainText);
		dlg->setText(msg);
		std::stringstream title;
		switch (dialog_type) {
		case JSDIALOGTYPE_CONFIRM:
			title << obs_module_text("Dialog.Confirm");
			dlg->setIcon(QMessageBox::Question);
			dlg->addButton(QMessageBox::Cancel);
			break;
		case JSDIALOGTYPE_ALERT:
		default:
			title << obs_module_text("Dialog.Alert");
			dlg->setIcon(QMessageBox::Information);
			break;
		}
		title << ": " << obs_module_text("Dialog.BrowserDock");
		dlg->setWindowTitle(title.str().c_str());

		auto finished = [callback](int result) {
			callback.get()->Continue(result == QMessageBox::Ok, "");
		};

		QWidget::connect(dlg, &QMessageBox::finished, finished);

		dlg->open();
	};
	QMetaObject::invokeMethod(QCoreApplication::instance()->thread(),
				  msgbox);
	return true;
#else
	UNUSED_PARAMETER(dialog_type);
	UNUSED_PARAMETER(message_text);
	UNUSED_PARAMETER(default_prompt_text);
	UNUSED_PARAMETER(callback);
	return false;
#endif
}

bool QCefBrowserClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
				      const CefKeyEvent &event, CefEventHandle,
				      bool *)
{
	if (event.type != KEYEVENT_RAWKEYDOWN)
		return false;

	if (event.windows_key_code == 'R' &&
#ifdef __APPLE__
	    (event.modifiers & EVENTFLAG_COMMAND_DOWN) != 0) {
#else
	    (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0) {
#endif
		browser->ReloadIgnoreCache();
		return true;
	} else if ((event.windows_key_code == 189 ||
		    event.windows_key_code == 109) &&
		   (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0) {
		// Zoom out
		return widget->zoomPage(-1);
	} else if ((event.windows_key_code == 187 ||
		    event.windows_key_code == 107) &&
		   (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0) {
		// Zoom in
		return widget->zoomPage(1);
	} else if ((event.windows_key_code == 48 ||
		    event.windows_key_code == 96) &&
		   (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0) {
		// Reset zoom
		return widget->zoomPage(0);
	}
	return false;
}

inline bool QCefBrowserClient::valid() const
{
	return !!widget && widget->isEnabled();
}

bool QCefBrowserClient::OnProcessMessageReceived(
	CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, CefProcessId,
	CefRefPtr<CefProcessMessage> message)
{
	const std::string &name = message->GetName();
	CefRefPtr<CefListValue> input_args = message->GetArgumentList();
	Json json;

	if (!valid()) {
		return false;
	}

	auto webpage_control_level = widget->controlLevel;

	// Fall-through switch, so that higher levels also have lower-level rights
	switch (webpage_control_level) {
	case ControlLevel::All:
		if (name == "startRecording") {
			obs_frontend_recording_start();
		} else if (name == "stopRecording") {
			obs_frontend_recording_stop();
		} else if (name == "startStreaming") {
			obs_frontend_streaming_start();
		} else if (name == "stopStreaming") {
			obs_frontend_streaming_stop();
		} else if (name == "pauseRecording") {
			obs_frontend_recording_pause(true);
		} else if (name == "unpauseRecording") {
			obs_frontend_recording_pause(false);
		} else if (name == "startVirtualcam") {
			obs_frontend_start_virtualcam();
		} else if (name == "stopVirtualcam") {
			obs_frontend_stop_virtualcam();
		}
		[[fallthrough]];
	case ControlLevel::Advanced:
		if (name == "startReplayBuffer") {
			obs_frontend_replay_buffer_start();
		} else if (name == "stopReplayBuffer") {
			obs_frontend_replay_buffer_stop();
		} else if (name == "setCurrentScene") {
			const std::string scene_name =
				input_args->GetString(1).ToString();
			OBSSourceAutoRelease source =
				obs_get_source_by_name(scene_name.c_str());
			if (!source) {
				blog(LOG_WARNING,
				     "Browser Widget '%s' tried to switch to scene '%s' which doesn't exist",
				     widget->url, scene_name.c_str());
			} else if (!obs_source_is_scene(source)) {
				blog(LOG_WARNING,
				     "Browser Widget '%s' tried to switch to '%s' which isn't a scene",
				     widget->url, scene_name.c_str());
			} else {
				obs_frontend_set_current_scene(source);
			}
		} else if (name == "setCurrentTransition") {
			const std::string transition_name =
				input_args->GetString(1).ToString();
			obs_frontend_source_list transitions = {};
			obs_frontend_get_transitions(&transitions);

			OBSSourceAutoRelease transition;
			for (size_t i = 0; i < transitions.sources.num; i++) {
				obs_source_t *source =
					transitions.sources.array[i];
				if (obs_source_get_name(source) ==
				    transition_name) {
					transition = obs_source_get_ref(source);
					break;
				}
			}

			obs_frontend_source_list_free(&transitions);

			if (transition)
				obs_frontend_set_current_transition(transition);
			else
				blog(LOG_WARNING,
				     "Browser Widget '%s' tried to change the current transition to '%s' which doesn't exist",
				     widget->url, transition_name.c_str());
		}
		[[fallthrough]];
	case ControlLevel::Basic:
		if (name == "saveReplayBuffer") {
			obs_frontend_replay_buffer_save();
		}
		[[fallthrough]];
	case ControlLevel::ReadUser:
		if (name == "getScenes") {
			struct obs_frontend_source_list list = {};
			obs_frontend_get_scenes(&list);
			std::vector<const char *> scenes_vector;
			for (size_t i = 0; i < list.sources.num; i++) {
				obs_source_t *source = list.sources.array[i];
				scenes_vector.push_back(
					obs_source_get_name(source));
			}
			json = scenes_vector;
			obs_frontend_source_list_free(&list);
		} else if (name == "getCurrentScene") {
			OBSSourceAutoRelease current_scene =
				obs_frontend_get_current_scene();

			if (!current_scene)
				return false;

			const char *name = obs_source_get_name(current_scene);
			if (!name)
				return false;

			json = Json::object{
				{"name", name},
				{"width",
				 (int)obs_source_get_width(current_scene)},
				{"height",
				 (int)obs_source_get_height(current_scene)}};
		} else if (name == "getTransitions") {
			struct obs_frontend_source_list list = {};
			obs_frontend_get_transitions(&list);
			std::vector<const char *> transitions_vector;
			for (size_t i = 0; i < list.sources.num; i++) {
				obs_source_t *source = list.sources.array[i];
				transitions_vector.push_back(
					obs_source_get_name(source));
			}
			json = transitions_vector;
			obs_frontend_source_list_free(&list);
		} else if (name == "getCurrentTransition") {
			OBSSourceAutoRelease source =
				obs_frontend_get_current_transition();
			json = obs_source_get_name(source);
		}
		[[fallthrough]];
	case ControlLevel::ReadObs:
		if (name == "getStatus") {
			json = Json::object{
				{"recording", obs_frontend_recording_active()},
				{"streaming", obs_frontend_streaming_active()},
				{"recordingPaused",
				 obs_frontend_recording_paused()},
				{"replaybuffer",
				 obs_frontend_replay_buffer_active()},
				{"virtualcam",
				 obs_frontend_virtualcam_active()}};
		}
		[[fallthrough]];
	case ControlLevel::None:
		if (name == "getControlLevel") {
			json = (int)webpage_control_level;
		}
	}

	CefRefPtr<CefProcessMessage> msg =
		CefProcessMessage::Create("executeCallback");

	CefRefPtr<CefListValue> execute_args = msg->GetArgumentList();
	execute_args->SetInt(0, input_args->GetInt(0));
	execute_args->SetString(1, json.dump());

	SendBrowserProcessMessage(browser, PID_RENDERER, msg);

	return true;
}
