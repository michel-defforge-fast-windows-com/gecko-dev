/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

var EXPORTED_SYMBOLS = [
  "ExtensionActionHelper",
  "GeckoViewConnection",
  "GeckoViewWebExtension",
];

const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);
const { GeckoViewUtils } = ChromeUtils.import(
  "resource://gre/modules/GeckoViewUtils.jsm"
);
const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");

XPCOMUtils.defineLazyModuleGetters(this, {
  AddonManager: "resource://gre/modules/AddonManager.jsm",
  EventDispatcher: "resource://gre/modules/Messaging.jsm",
  Extension: "resource://gre/modules/Extension.jsm",
  ExtensionChild: "resource://gre/modules/ExtensionChild.jsm",
  GeckoViewTabBridge: "resource://gre/modules/GeckoViewTab.jsm",
});

XPCOMUtils.defineLazyServiceGetter(
  this,
  "mimeService",
  "@mozilla.org/mime;1",
  "nsIMIMEService"
);

const { debug, warn } = GeckoViewUtils.initLogging("Console"); // eslint-disable-line no-unused-vars

/** Provides common logic between page and browser actions */
class ExtensionActionHelper {
  constructor({
    tabTracker,
    windowTracker,
    tabContext,
    properties,
    extension,
  }) {
    this.tabTracker = tabTracker;
    this.windowTracker = windowTracker;
    this.tabContext = tabContext;
    this.properties = properties;
    this.extension = extension;
  }

  getTab(aTabId) {
    if (aTabId !== null) {
      return this.tabTracker.getTab(aTabId);
    }
    return null;
  }

  getWindow(aWindowId) {
    if (aWindowId !== null) {
      return this.windowTracker.getWindow(aWindowId);
    }
    return null;
  }

  extractProperties(aAction) {
    const merged = {};
    for (const p of this.properties) {
      merged[p] = aAction[p];
    }
    return merged;
  }

  eventDispatcherFor(aTabId) {
    if (!aTabId) {
      return EventDispatcher.instance;
    }

    const windowId = GeckoViewTabBridge.tabIdToWindowId(aTabId);
    const window = this.windowTracker.getWindow(windowId);
    return window.WindowEventDispatcher;
  }

  sendRequestForResult(aTabId, aData) {
    return this.eventDispatcherFor(aTabId).sendRequestForResult({
      ...aData,
      aTabId,
      extensionId: this.extension.id,
    });
  }

  sendRequest(aTabId, aData) {
    return this.eventDispatcherFor(aTabId).sendRequest({
      ...aData,
      aTabId,
      extensionId: this.extension.id,
    });
  }
}

class EmbedderPort extends ExtensionChild.Port {
  constructor(...args) {
    super(...args);
    EventDispatcher.instance.registerListener(this, [
      "GeckoView:WebExtension:PortMessageFromApp",
      "GeckoView:WebExtension:PortDisconnect",
    ]);
  }
  handleDisconnection() {
    super.handleDisconnection();
    EventDispatcher.instance.unregisterListener(this, [
      "GeckoView:WebExtension:PortMessageFromApp",
      "GeckoView:WebExtension:PortDisconnect",
    ]);
  }
  close() {
    // Notify listeners that this port is being closed because the context is
    // gone.
    this.disconnectByOtherEnd();
  }
  onEvent(aEvent, aData, aCallback) {
    debug`onEvent ${aEvent} ${aData}`;

    if (this.id !== aData.portId) {
      return;
    }

    switch (aEvent) {
      case "GeckoView:WebExtension:PortMessageFromApp": {
        this.postMessage(aData.message);
        break;
      }

      case "GeckoView:WebExtension:PortDisconnect": {
        this.disconnect();
        break;
      }
    }
  }
}

class GeckoViewConnection {
  constructor(context, sender, target, nativeApp) {
    this.context = context;
    this.sender = sender;
    this.target = target;
    this.nativeApp = nativeApp;
    this.allowContentMessaging = GeckoViewWebExtension.extensionScopes.get(
      sender.extensionId
    ).allowContentMessaging;
  }

  _getMessageManager(aTarget) {
    if (aTarget.frameLoader) {
      return aTarget.frameLoader.messageManager;
    }
    return aTarget;
  }

  get dispatcher() {
    if (this.sender.envType === "addon_child") {
      // If this is a WebExtension Page we will have a GeckoSession associated
      // to it and thus a dispatcher.
      const dispatcher = GeckoViewUtils.getDispatcherForWindow(
        this.target.ownerGlobal
      );
      if (dispatcher) {
        return dispatcher;
      }

      // No dispatcher means this message is coming from a background script,
      // use the global event handler
      return EventDispatcher.instance;
    } else if (
      this.sender.envType === "content_child" &&
      this.allowContentMessaging
    ) {
      // If this message came from a content script, send the message to
      // the corresponding tab messenger so that GeckoSession can pick it
      // up.
      return GeckoViewUtils.getDispatcherForWindow(this.target.ownerGlobal);
    }

    throw new Error(`Uknown sender envType: ${this.sender.envType}`);
  }

  _sendMessage({ type, portId, data }) {
    const message = {
      type,
      sender: this.sender,
      data,
      portId,
      nativeApp: this.nativeApp,
    };

    return this.dispatcher.sendRequestForResult(message);
  }

  sendMessage(data) {
    return this._sendMessage({
      type: "GeckoView:WebExtension:Message",
      data: data.deserialize({}),
    });
  }

  onConnect(portId) {
    const port = new EmbedderPort(
      this.context,
      this.target.messageManager,
      [Services.mm],
      "",
      portId,
      this.sender,
      this.sender
    );
    port.registerOnMessage(holder =>
      this._sendMessage({
        type: "GeckoView:WebExtension:PortMessage",
        portId: port.id,
        data: holder.deserialize({}),
      })
    );

    port.registerOnDisconnect(msg =>
      EventDispatcher.instance.sendRequest({
        type: "GeckoView:WebExtension:Disconnect",
        sender: this.sender,
        portId: port.id,
      })
    );

    return this._sendMessage({
      type: "GeckoView:WebExtension:Connect",
      data: {},
      portId: port.id,
    });
  }
}

function exportExtension(aAddon, aPermissions, aSourceURI) {
  const { origins, permissions } = aPermissions;
  const {
    creator,
    description,
    homepageURL,
    signedState,
    name,
    icons,
    version,
    optionsURL,
    optionsBrowserStyle,
    isRecommended,
    blocklistState,
    isActive,
    isBuiltin,
    id,
  } = aAddon;
  let creatorName = null;
  let creatorURL = null;
  if (creator) {
    const { name, url } = creator;
    creatorName = name;
    creatorURL = url;
  }
  const openOptionsPageInTab =
    optionsBrowserStyle === AddonManager.OPTIONS_TYPE_TAB;
  return {
    webExtensionId: id,
    locationURI: aSourceURI != null ? aSourceURI.spec : "",
    isEnabled: isActive,
    isBuiltIn: isBuiltin,
    metaData: {
      permissions,
      origins,
      description,
      version,
      creatorName,
      creatorURL,
      homepageURL,
      name,
      optionsPageURL: optionsURL,
      openOptionsPageInTab,
      isRecommended,
      blocklistState,
      signedState,
      icons,
    },
  };
}

class ExtensionInstallListener {
  constructor(aResolve) {
    this.resolve = aResolve;
  }

  onDownloadCancelled(aInstall) {
    const { error: installError } = aInstall;
    this.resolve({ installError });
  }

  onDownloadFailed(aInstall) {
    const { error: installError } = aInstall;
    this.resolve({ installError });
  }

  onDownloadEnded() {
    // Nothing to do
  }

  onInstallCancelled(aInstall) {
    const { error: installError } = aInstall;
    this.resolve({ installError });
  }

  onInstallFailed(aInstall) {
    const { error: installError } = aInstall;
    this.resolve({ installError });
  }

  onInstallEnded(aInstall, aAddon) {
    const extension = exportExtension(
      aAddon,
      aAddon.userPermissions,
      aInstall.sourceURI
    );
    this.resolve({ extension });
  }
}

class ExtensionPromptObserver {
  constructor() {
    Services.obs.addObserver(this, "webextension-permission-prompt");
  }

  async permissionPrompt(aInstall, aAddon, aInfo) {
    const { sourceURI } = aInstall;
    const { permissions } = aInfo;
    const extension = exportExtension(aAddon, permissions, sourceURI);
    const response = await EventDispatcher.instance.sendRequestForResult({
      type: "GeckoView:WebExtension:InstallPrompt",
      extension,
    });

    if (response.allow) {
      aInfo.resolve();
    } else {
      aInfo.reject();
    }
  }

  observe(aSubject, aTopic, aData) {
    debug`observe ${aTopic}`;

    switch (aTopic) {
      case "webextension-permission-prompt": {
        const { info } = aSubject.wrappedJSObject;
        const { addon, install } = info;
        this.permissionPrompt(install, addon, info);
        break;
      }
    }
  }
}

new ExtensionPromptObserver();

var GeckoViewWebExtension = {
  async registerWebExtension(aId, aUri, allowContentMessaging, aCallback) {
    const params = {
      id: aId,
      resourceURI: aUri,
      temporarilyInstalled: true,
      builtIn: true,
    };

    let file;
    if (aUri instanceof Ci.nsIFileURL) {
      file = aUri.file;
    }

    const scope = Extension.getBootstrapScope(aId, file);
    scope.allowContentMessaging = allowContentMessaging;
    this.extensionScopes.set(aId, scope);

    await scope.startup(params, undefined);

    scope.extension.callOnClose({
      close: () => this.extensionScopes.delete(aId),
    });
  },

  async unregisterWebExtension(aId, aCallback) {
    try {
      const scope = this.extensionScopes.get(aId);
      await scope.shutdown();
      this.extensionScopes.delete(aId);
      aCallback.onSuccess();
    } catch (ex) {
      aCallback.onError(`Error unregistering WebExtension ${aId}. ${ex}`);
    }
  },

  async extensionById(aId) {
    let scope = this.extensionScopes.get(aId);
    if (!scope) {
      // Check if this is an installed extension we haven't seen yet
      const addon = await AddonManager.getAddonByID(aId);
      if (!addon) {
        debug`Could not find extension with id=${aId}`;
        return null;
      }
      scope = {
        allowContentMessaging: false,
        extension: addon,
      };
    }

    return scope.extension;
  },

  async installWebExtension(aUri) {
    const install = await AddonManager.getInstallForURL(aUri.spec);
    const promise = new Promise(resolve => {
      install.addListener(new ExtensionInstallListener(resolve));
    });

    const systemPrincipal = Services.scriptSecurityManager.getSystemPrincipal();
    const mimeType = mimeService.getTypeFromURI(aUri);
    AddonManager.installAddonFromWebpage(
      mimeType,
      null,
      systemPrincipal,
      install
    );

    return promise;
  },

  async uninstallWebExtension(aId) {
    const extension = await this.extensionById(aId);
    if (!extension) {
      throw new Error(`Could not find an extension with id='${aId}'.`);
    }

    return extension.uninstall();
  },

  async browserActionClick(aId) {
    const extension = await this.extensionById(aId);
    if (!extension) {
      return;
    }

    const browserAction = this.browserActions.get(extension);
    if (!browserAction) {
      return;
    }

    browserAction.click();
  },

  async pageActionClick(aId) {
    const extension = await this.extensionById(aId);
    if (!extension) {
      return;
    }

    const pageAction = this.pageActions.get(extension);
    if (!pageAction) {
      return;
    }

    pageAction.click();
  },

  async actionDelegateAttached(aId) {
    const extension = await this.extensionById(aId);
    if (!extension) {
      return;
    }

    const browserAction = this.browserActions.get(extension);
    if (browserAction) {
      // Send information about this action to the delegate
      browserAction.updateOnChange(null);
    }

    const pageAction = this.pageActions.get(extension);
    if (pageAction) {
      pageAction.updateOnChange(null);
    }
  },

  async onEvent(aEvent, aData, aCallback) {
    debug`onEvent ${aEvent} ${aData}`;

    switch (aEvent) {
      case "GeckoView:BrowserAction:Click": {
        this.browserActionClick(aData.extensionId);
        break;
      }
      case "GeckoView:PageAction:Click": {
        this.pageActionClick(aData.extensionId);
        break;
      }
      case "GeckoView:RegisterWebExtension": {
        const uri = Services.io.newURI(aData.locationUri);
        if (
          uri == null ||
          (!(uri instanceof Ci.nsIFileURL) && !(uri instanceof Ci.nsIJARURI))
        ) {
          aCallback.onError(
            `Extension does not point to a resource URI or a file URL. extension=${
              aData.locationUri
            }`
          );
          return;
        }

        if (uri.fileName != "" && uri.fileExtension != "xpi") {
          aCallback.onError(
            `Extension does not point to a folder or an .xpi file. Hint: the path needs to end with a "/" to be considered a folder. extension=${
              aData.locationUri
            }`
          );
          return;
        }

        if (this.extensionScopes.has(aData.id)) {
          aCallback.onError(
            `An extension with id='${aData.id}' has already been registered.`
          );
          return;
        }

        this.registerWebExtension(
          aData.id,
          uri,
          aData.allowContentMessaging
        ).then(aCallback.onSuccess, error =>
          aCallback.onError(
            `An error occurred while registering the WebExtension ${
              aData.locationUri
            }: ${error}.`
          )
        );
        break;
      }

      case "GeckoView:ActionDelegate:Attached": {
        this.actionDelegateAttached(aData.extensionId);
        break;
      }

      case "GeckoView:UnregisterWebExtension": {
        if (!this.extensionScopes.has(aData.id)) {
          aCallback.onError(
            `Could not find an extension with id='${aData.id}'.`
          );
          return;
        }

        this.unregisterWebExtension(aData.id, aCallback);
        break;
      }

      case "GeckoView:WebExtension:Get": {
        const extension = await this.extensionById(aData.extensionId);
        if (!extension) {
          aCallback.onError(
            `Could not find extension with id: ${aData.extensionId}`
          );
          return;
        }

        aCallback.onSuccess({
          extension: exportExtension(
            extension,
            extension.userPermissions,
            /* aSourceURI */ null
          ),
        });
        break;
      }

      case "GeckoView:WebExtension:Install": {
        const uri = Services.io.newURI(aData.locationUri);
        if (uri == null) {
          aCallback.onError(`Could not parse uri: ${uri}`);
          return;
        }

        try {
          const result = await this.installWebExtension(uri);
          if (result.extension) {
            aCallback.onSuccess(result);
          } else {
            aCallback.onError(result);
          }
        } catch (ex) {
          debug`Install exception error ${ex}`;
          aCallback.onError(`Unexpected error: ${ex}`);
        }

        break;
      }

      case "GeckoView:WebExtension:InstallBuiltIn": {
        // TODO
        aCallback.onError(`Not implemented`);
        break;
      }

      case "GeckoView:WebExtension:Uninstall": {
        try {
          await this.uninstallWebExtension(aData.webExtensionId);
          aCallback.onSuccess();
        } catch (ex) {
          debug`Failed uninstall ${ex}`;
          aCallback.onError(
            `This extension cannot be uninstalled. Error: ${ex}.`
          );
        }
        break;
      }

      case "GeckoView:WebExtension:Enable": {
        // TODO
        aCallback.onError(`Not implemented`);
        break;
      }

      case "GeckoView:WebExtension:Disable": {
        // TODO
        aCallback.onError(`Not implemented`);
        break;
      }

      case "GeckoView:WebExtension:List": {
        // TODO
        aCallback.onError(`Not implemented`);
        break;
      }

      case "GeckoView:WebExtension:Update": {
        // TODO
        aCallback.onError(`Not implemented`);
        break;
      }
    }
  },
};

GeckoViewWebExtension.extensionScopes = new Map();
// WeakMap[Extension -> BrowserAction]
GeckoViewWebExtension.browserActions = new WeakMap();
// WeakMap[Extension -> PageAction]
GeckoViewWebExtension.pageActions = new WeakMap();
