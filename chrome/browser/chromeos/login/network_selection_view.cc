// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/network_selection_view.h"

#include <signal.h>
#include <sys/types.h>

#include <string>

#include "app/l10n_util.h"
#include "app/resource_bundle.h"
#include "chrome/browser/chromeos/login/screen_observer.h"
#include "grit/chromium_strings.h"
#include "grit/generated_resources.h"
#include "views/controls/button/native_button.h"
#include "views/controls/combobox/combobox.h"
#include "views/controls/label.h"
#include "views/widget/widget.h"
#include "views/window/non_client_view.h"
#include "views/window/window.h"
#include "views/window/window_gtk.h"

using views::Background;
using views::Label;
using views::View;
using views::Widget;

namespace {

const int kCornerRadius = 12;
const int kShadow = 10;
const int kWelcomeLabelY = 150;
const int kOfflineButtonX = 30;
const int kSpacing = 25;
const int kComboboxSpacing = 5;
const int kHorizontalSpacing = 25;
const int kNetworkComboboxWidth = 250;
const int kNetworkComboboxHeight = 30;
const SkColor kWelcomeColor = 0x0054A4;
const SkColor kBackground = SK_ColorWHITE;
const SkColor kShadowColor = 0x40223673;

}  // namespace

NetworkSelectionView::NetworkSelectionView(chromeos::ScreenObserver* observer)
    : network_combobox_(NULL),
      welcome_label_(NULL),
      select_network_label_(NULL),
      observer_(observer) {
  chromeos::NetworkLibrary::Get()->AddObserver(this);
}

NetworkSelectionView::~NetworkSelectionView() {
  chromeos::NetworkLibrary::Get()->RemoveObserver(this);
}

void NetworkSelectionView::Init() {
  // TODO(nkostylev): Add UI language and logo.
  // Use rounded rect background.
  views::Painter* painter = new chromeos::RoundedRectPainter(
      0, 0x00000000,              // no padding
      kShadow, kShadowColor,      // gradient shadow
      kCornerRadius,              // corner radius
      kBackground, kBackground);  // backgound without gradient
  set_background(
      views::Background::CreateBackgroundPainter(true, painter));

  gfx::Font welcome_label_font =
      gfx::Font::CreateFont(L"Droid Sans", 20).DeriveFont(0, gfx::Font::BOLD);
  gfx::Font network_label_font = gfx::Font::CreateFont(L"Droid Sans", 9);
  gfx::Font button_font = network_label_font;

  welcome_label_ = new views::Label();
  welcome_label_->SetColor(kWelcomeColor);
  welcome_label_->SetFont(welcome_label_font);

  select_network_label_ = new views::Label();
  select_network_label_->SetFont(network_label_font);

  network_combobox_ = new views::Combobox(this);
  network_combobox_->set_listener(this);

  offline_button_ = new views::NativeButton(this, std::wstring());
  offline_button_->set_font(button_font);

  UpdateLocalizedStrings();

  AddChildView(welcome_label_);
  AddChildView(select_network_label_);
  AddChildView(network_combobox_);
  AddChildView(offline_button_);
}

void NetworkSelectionView::UpdateLocalizedStrings() {
  welcome_label_->SetText(l10n_util::GetStringF(IDS_NETWORK_SELECTION_TITLE,
                          l10n_util::GetString(IDS_PRODUCT_OS_NAME)));
  select_network_label_->SetText(
      l10n_util::GetString(IDS_NETWORK_SELECTION_SELECT));
  offline_button_->SetLabel(
      l10n_util::GetString(IDS_NETWORK_SELECTION_OFFLINE_BUTTON));
  NetworkChanged(chromeos::NetworkLibrary::Get());
}

////////////////////////////////////////////////////////////////////////////////
// views::View: implementation:

gfx::Size NetworkSelectionView::GetPreferredSize() {
  return gfx::Size(width(), height());
}

void NetworkSelectionView::Layout() {
  int x = (width() -
           welcome_label_->GetPreferredSize().width()) / 2;
  int y = kWelcomeLabelY;
  welcome_label_->SetBounds(x,
                            y,
                            welcome_label_->GetPreferredSize().width(),
                            welcome_label_->GetPreferredSize().height());

  x = (width() - select_network_label_->GetPreferredSize().width() -
       kNetworkComboboxWidth) / 2;
  y += welcome_label_->GetPreferredSize().height() + kSpacing;
  select_network_label_->SetBounds(
      x,
      y,
      select_network_label_->GetPreferredSize().width(),
      select_network_label_->GetPreferredSize().height());

  x += select_network_label_->GetPreferredSize().width() + kHorizontalSpacing;
  y -= kComboboxSpacing;
  network_combobox_->SetBounds(x, y,
                               kNetworkComboboxWidth, kNetworkComboboxHeight);

  y = height() - offline_button_->GetPreferredSize().height() - kSpacing;
  offline_button_->SetBounds(
      kOfflineButtonX,
      y,
      offline_button_->GetPreferredSize().width(),
      offline_button_->GetPreferredSize().height());

  // Need to refresh combobox layout explicitly.
  network_combobox_->Layout();
  SchedulePaint();
}

////////////////////////////////////////////////////////////////////////////////
// ComboboxModel implementation:

int NetworkSelectionView::GetItemCount() {
  // Item with index = 0 is either "no networks are available" or
  // "no selection".
  return static_cast<int>(networks_.GetNetworkCount()) + 1;
}

std::wstring NetworkSelectionView::GetItemAt(int index) {
  if (index == 0) {
    return networks_.IsEmpty() ?
        l10n_util::GetString(IDS_STATUSBAR_NO_NETWORKS_MESSAGE) :
        l10n_util::GetString(IDS_NETWORK_SELECTION_NONE);
  }
  chromeos::NetworkList::NetworkItem* network =
      networks_.GetNetworkAt(index - 1);
  return network ? UTF16ToWide(network->label) : std::wstring();
}

////////////////////////////////////////////////////////////////////////////////
// views::Combobox::Listener implementation:

void NetworkSelectionView::ItemChanged(views::Combobox* sender,
                                       int prev_index,
                                       int new_index) {
  if (new_index == prev_index || new_index < 0 || prev_index < 0)
    return;

  // First item is a text, not a network.
  if (new_index == 0) {
    network_combobox_->SetSelectedItem(prev_index);
    return;
  }

  if (networks_.IsEmpty())
    return;

  chromeos::NetworkList::NetworkItem* network =
      networks_.GetNetworkAt(new_index - 1);
  if (network) {
    if (chromeos::NetworkList::NETWORK_WIFI == network->network_type) {
      if (network->wifi_network.encrypted) {
        OpenPasswordDialog(network->wifi_network);
        return;
      } else {
        chromeos::NetworkLibrary::Get()->ConnectToWifiNetwork(
            network->wifi_network, string16());
      }
    } else if (chromeos::NetworkList::NETWORK_CELLULAR ==
               network->network_type) {
      chromeos::NetworkLibrary::Get()->ConnectToCellularNetwork(
          network->cellular_network);
    }
    // TODO(avayvod): Check for connection error.
    NotifyOnConnection();
  }
}

///////////////////////////////////////////////////////////////////////////////
// views::ButtonListener implementation:
void NetworkSelectionView::ButtonPressed(views::Button* sender,
                                         const views::Event& event) {
  if (observer_) {
    observer_->OnExit(chromeos::ScreenObserver::NETWORK_OFFLINE);
  }
}

////////////////////////////////////////////////////////////////////////////////
// PasswordDialogDelegate implementation:

bool NetworkSelectionView::OnPasswordDialogAccept(const std::string& ssid,
                                                  const string16& password) {
  chromeos::NetworkList::NetworkItem* network =
      networks_.GetNetworkById(chromeos::NetworkList::NETWORK_WIFI,
                               ASCIIToUTF16(ssid));
  if (network &&
      chromeos::NetworkList::NETWORK_WIFI == network->network_type) {
    chromeos::NetworkLibrary::Get()->ConnectToWifiNetwork(network->wifi_network,
                                                          password);
    NotifyOnConnection();
  }
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// NetworkLibrary::Observer implementation:

void NetworkSelectionView::NetworkChanged(
    chromeos::NetworkLibrary* network_lib) {
  // Save network selection in case it would be available after refresh.
  chromeos::NetworkList::NetworkType network_type =
    chromeos::NetworkList::NETWORK_EMPTY;
  string16 network_id;
  chromeos::NetworkList::NetworkItem* network = GetSelectedNetwork();
  if (network) {
    network_type = network->network_type;
    network_id = network->label;
  }
  networks_.NetworkChanged(network_lib);
  network_combobox_->ModelChanged();
  SelectNetwork(network_type, network_id);
}

void NetworkSelectionView::NetworkTraffic(chromeos::NetworkLibrary* cros,
                                          int traffic_type) {
}

////////////////////////////////////////////////////////////////////////////////
// NetworkSelectionView, private:

chromeos::NetworkList::NetworkItem* NetworkSelectionView::GetSelectedNetwork() {
  return networks_.GetNetworkAt(network_combobox_->selected_item() - 1);
}

void NetworkSelectionView::NotifyOnConnection() {
  if (observer_) {
    observer_->OnExit(chromeos::ScreenObserver::NETWORK_CONNECTED);
  }
}

void NetworkSelectionView::OpenPasswordDialog(chromeos::WifiNetwork network) {
  // TODO(nkostylev): Reuse this code in network menu button.
  chromeos::PasswordDialogView* dialog = new chromeos::PasswordDialogView(
      this, network.ssid);
  views::Window* window = views::Window::CreateChromeWindow(
      GetWindow()->GetNativeWindow(), gfx::Rect(), dialog);
  gfx::Size size = dialog->GetPreferredSize();
  gfx::Rect rect = bounds();
  gfx::Point point = gfx::Point(rect.width() - size.width(), rect.height());
  ConvertPointToScreen(this, &point);
  window->SetBounds(gfx::Rect(point, size), GetWindow()->GetNativeWindow());
  window->Show();
}

void NetworkSelectionView::SelectNetwork(
    chromeos::NetworkList::NetworkType type, const string16& id) {
  int index = networks_.GetNetworkIndexById(type, id);
  if (index >= 0) {
    network_combobox_->SetSelectedItem(index + 1);
  } else {
    network_combobox_->SetSelectedItem(0);
  }
}
