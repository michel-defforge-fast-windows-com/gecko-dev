package org.mozilla.geckoview.test

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.support.test.InstrumentationRegistry
import android.support.test.filters.MediumTest
import org.hamcrest.Matchers.equalTo
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeThat
import org.junit.Before
import org.junit.Ignore
import org.junit.Test
import org.junit.runner.RunWith
import org.junit.runners.Parameterized
import org.mozilla.geckoview.GeckoResult
import org.mozilla.geckoview.GeckoSession
import org.mozilla.geckoview.WebExtension
import org.mozilla.geckoview.test.rule.GeckoSessionTestRule

@MediumTest
@RunWith(Parameterized::class)
class ExtensionActionTest : BaseSessionTest() {
    var extension: WebExtension? = null
    var default: WebExtension.Action? = null
    var backgroundPort: WebExtension.Port? = null
    var windowPort: WebExtension.Port? = null

    companion object {
        @get:Parameterized.Parameters(name = "{0}")
        @JvmStatic
        val parameters: List<Array<out Any>> = listOf(
                arrayOf("#pageAction"),
                arrayOf("#browserAction"))
    }

    @field:Parameterized.Parameter(0) @JvmField var id: String = ""

    @Before
    fun setup() {
        // This method installs the extension, opens up ports with the background script and the
        // content script and captures the default action definition from the manifest
        val browserActionDefaultResult = GeckoResult<WebExtension.Action>()
        val pageActionDefaultResult = GeckoResult<WebExtension.Action>()

        val windowPortResult = GeckoResult<WebExtension.Port>()
        val backgroundPortResult = GeckoResult<WebExtension.Port>()

        extension = WebExtension("resource://android/assets/web_extensions/actions/",
                "actions", WebExtension.Flags.ALLOW_CONTENT_MESSAGING)

        sessionRule.session.setMessageDelegate(
                extension!!,
                object : WebExtension.MessageDelegate {
                    override fun onConnect(port: WebExtension.Port) {
                        windowPortResult.complete(port)
                    }
                }, "browser")
        extension!!.setMessageDelegate(object : WebExtension.MessageDelegate {
            override fun onConnect(port: WebExtension.Port) {
                backgroundPortResult.complete(port)
            }
        }, "browser")

        sessionRule.addExternalDelegateDuringNextWait(
                WebExtension.ActionDelegate::class,
                extension!!::setActionDelegate,
                { extension!!.setActionDelegate(null) },
        object : WebExtension.ActionDelegate {
            override fun onBrowserAction(extension: WebExtension, session: GeckoSession?, action: WebExtension.Action) {
                assertEquals(action.title, "Test action default")
                browserActionDefaultResult.complete(action)
            }
            override fun onPageAction(extension: WebExtension, session: GeckoSession?, action: WebExtension.Action) {
                assertEquals(action.title, "Test action default")
                pageActionDefaultResult.complete(action)
            }
        })

        sessionRule.waitForResult(sessionRule.runtime.registerWebExtension(extension!!))

        sessionRule.session.loadUri("http://example.com")
        sessionRule.waitForPageStop()

        default = when (id) {
            "#pageAction" -> sessionRule.waitForResult(pageActionDefaultResult)
            "#browserAction" -> sessionRule.waitForResult(browserActionDefaultResult)
            else -> throw IllegalArgumentException()
        }

        windowPort = sessionRule.waitForResult(windowPortResult)
        backgroundPort = sessionRule.waitForResult(backgroundPortResult)

        if (id == "#pageAction") {
            // Make sure that the pageAction starts enabled for this tab
            testActionApi("""{"action": "enable"}""") { action ->
                assertEquals(action.enabled, true)
            }
        }
    }

    private var type: String = ""
        get() = when(id) {
            "#pageAction" -> "pageAction"
            "#browserAction" -> "browserAction"
            else -> throw IllegalArgumentException()
        }

    @After
    fun tearDown() {
        sessionRule.waitForResult(sessionRule.runtime.unregisterWebExtension(extension!!))
    }

    private fun testBackgroundActionApi(message: String, tester: (WebExtension.Action) -> Unit) {
        val result = GeckoResult<Void>()

        val json = JSONObject(message)
        json.put("type", type)

        backgroundPort!!.postMessage(json)

        sessionRule.addExternalDelegateDuringNextWait(
                WebExtension.ActionDelegate::class,
                extension!!::setActionDelegate,
                { extension!!.setActionDelegate(null) },
                object : WebExtension.ActionDelegate {
            override fun onBrowserAction(extension: WebExtension, session: GeckoSession?, action: WebExtension.Action) {
                if (sessionRule.currentCall.counter == 1) {
                    // When attaching the delegate, we will receive a default message, ignore it
                    return
                }
                assertEquals(id, "#browserAction")
                default = action
                tester(action)
                result.complete(null)
            }
            override fun onPageAction(extension: WebExtension, session: GeckoSession?, action: WebExtension.Action) {
                if (sessionRule.currentCall.counter == 1) {
                    // When attaching the delegate, we will receive a default message, ignore it
                    return
                }
                assertEquals(id, "#pageAction")
                default = action
                tester(action)
                result.complete(null)
            }
        })

        sessionRule.waitForResult(result)
    }

    private fun testActionApi(message: String, tester: (WebExtension.Action) -> Unit) {
        val result = GeckoResult<Void>()

        val json = JSONObject(message)
        json.put("type", type)

        windowPort!!.postMessage(json)

        sessionRule.addExternalDelegateDuringNextWait(
                WebExtension.ActionDelegate::class,
                { delegate ->
                    sessionRule.session.setWebExtensionActionDelegate(extension!!, delegate) },
                { sessionRule.session.setWebExtensionActionDelegate(extension!!, null) },
        object : WebExtension.ActionDelegate {
            override fun onBrowserAction(extension: WebExtension, session: GeckoSession?, action: WebExtension.Action) {
                assertEquals(id, "#browserAction")
                val resolved = action.withDefault(default!!)
                tester(resolved)
                result.complete(null)
            }
            override fun onPageAction(extension: WebExtension, session: GeckoSession?, action: WebExtension.Action) {
                assertEquals(id, "#pageAction")
                val resolved = action.withDefault(default!!)
                tester(resolved)
                result.complete(null)
            }
        })

        sessionRule.waitForResult(result)
    }

    @Test
    fun disableTest() {
        testActionApi("""{"action": "disable"}""") { action ->
            assertEquals(action.title, "Test action default")
            assertEquals(action.enabled, false)
        }
    }

    @Test
    fun attachingDelegateTriggersDefaultUpdate() {
        val result = GeckoResult<Void>()

        // We should always get a default update after we attach the delegate
        when (id) {
            "#browserAction" -> {
                extension!!.setActionDelegate(object : WebExtension.ActionDelegate {
                    override fun onBrowserAction(extension: WebExtension, session: GeckoSession?,
                                                 action: WebExtension.Action) {
                        assertEquals(action.title, "Test action default")
                        result.complete(null)
                    }
                })
            }
            "#pageAction" -> {
                extension!!.setActionDelegate(object : WebExtension.ActionDelegate {
                    override fun onPageAction(extension: WebExtension, session: GeckoSession?,
                                              action: WebExtension.Action) {
                        assertEquals(action.title, "Test action default")
                        result.complete(null)
                    }
                })
            }
            else -> throw IllegalArgumentException()
        }

        sessionRule.waitForResult(result)
    }

    @Test
    fun enableTest() {
        // First, make sure the action is disabled
        testActionApi("""{"action": "disable"}""") { action ->
            assertEquals(action.title, "Test action default")
            assertEquals(action.enabled, false)
        }

        testActionApi("""{"action": "enable"}""") { action ->
            assertEquals(action.title, "Test action default")
            assertEquals(action.enabled, true)
        }
    }

    @Test
    fun setOverridenTitle() {
        testActionApi("""{
               "action": "setTitle",
               "title": "overridden title"
            }""") { action ->
            assertEquals(action.title, "overridden title")
            assertEquals(action.enabled, true)
        }
    }

    @Test
    fun setBadgeText() {
        assumeThat("Only browserAction supports this API.", id, equalTo("#browserAction"))

        testActionApi("""{
           "action": "setBadgeText",
           "text": "12"
        }""") { action ->
            assertEquals(action.title, "Test action default")
            assertEquals(action.badgeText, "12")
            assertEquals(action.enabled, true)
        }
    }

    @Test
    fun setBadgeBackgroundColor() {
        assumeThat("Only browserAction supports this API.", id, equalTo("#browserAction"))

        colorTest("setBadgeBackgroundColor", "#ABCDEF", "#FFABCDEF")
        colorTest("setBadgeBackgroundColor", "#F0A", "#FFFF00AA")
        colorTest("setBadgeBackgroundColor", "red", "#FFFF0000")
        colorTest("setBadgeBackgroundColor", "rgb(0, 0, 255)", "#FF0000FF")
        colorTest("setBadgeBackgroundColor", "rgba(0, 255, 0, 0.5)", "#8000FF00")
        colorRawTest("setBadgeBackgroundColor", "[0, 0, 255, 128]", "#800000FF")
    }

    private fun colorTest(actionName: String, color: String, expectedHex: String) {
        colorRawTest(actionName, "\"$color\"", expectedHex)
    }

    private fun colorRawTest(actionName: String, color: String, expectedHex: String) {
        testActionApi("""{
           "action": "$actionName",
           "color": $color
        }""") { action ->
            assertEquals(action.title, "Test action default")
            assertEquals(action.badgeText, "")
            assertEquals(action.enabled, true)

            val result = when (actionName) {
                "setBadgeTextColor" -> action.badgeTextColor!!
                "setBadgeBackgroundColor" -> action.badgeBackgroundColor!!
                else -> throw IllegalArgumentException()
            }

            val hexColor = String.format("#%08X", result)
            assertEquals(hexColor, "$expectedHex")
        }
    }

    @Test
    fun setBadgeTextColor() {
        assumeThat("Only browserAction supports this API.", id, equalTo("#browserAction"))

        colorTest("setBadgeTextColor", "#ABCDEF", "#FFABCDEF")
        colorTest("setBadgeTextColor", "#F0A", "#FFFF00AA")
        colorTest("setBadgeTextColor", "red", "#FFFF0000")
        colorTest("setBadgeTextColor", "rgb(0, 0, 255)", "#FF0000FF")
        colorTest("setBadgeTextColor", "rgba(0, 255, 0, 0.5)", "#8000FF00")
        colorRawTest("setBadgeTextColor", "[0, 0, 255, 128]", "#800000FF")
    }

    @Test
    fun setDefaultTitle() {
        assumeThat("Only browserAction supports default properties.", id, equalTo("#browserAction"))

        // Setting a default value will trigger the default handler on the extension object
        testBackgroundActionApi("""{
            "action": "setTitle",
            "title": "new default title"
        }""") { action ->
            assertEquals(action.title, "new default title")
            assertEquals(action.badgeText, "")
            assertEquals(action.enabled, true)
        }

        // When an overridden title is set, the default has no effect
        testActionApi("""{
           "action": "setTitle",
           "title": "test override"
        }""") { action ->
            assertEquals(action.title, "test override")
            assertEquals(action.badgeText, "")
            assertEquals(action.enabled, true)
        }

        // When the override is null, the new default takes effect
        testActionApi("""{
           "action": "setTitle",
           "title": null
        }""") { action ->
            assertEquals(action.title, "new default title")
            assertEquals(action.badgeText, "")
            assertEquals(action.enabled, true)
        }

        // When the default value is null, the manifest value is used
        testBackgroundActionApi("""{
           "action": "setTitle",
           "title": null
        }""") { action ->
            assertEquals(action.title, "Test action default")
            assertEquals(action.badgeText, "")
            assertEquals(action.enabled, true)
        }
    }

    private fun compareBitmap(expectedLocation: String, actual: Bitmap) {
        val stream = InstrumentationRegistry.getTargetContext().assets
                .open(expectedLocation)

        val expected = BitmapFactory.decodeStream(stream)
        for (x in 0 until actual.height) {
            for (y in 0 until actual.width) {
                assertEquals(expected.getPixel(x, y), actual.getPixel(x, y))
            }
        }
    }

    @Test
    fun setIconSvg() {
        val svg = GeckoResult<Void>()

        testActionApi("""{
           "action": "setIcon",
           "path": "button/icon.svg"
        }""") { action ->
            assertEquals(action.title, "Test action default")
            assertEquals(action.enabled, true)

            action.icon!!.get(100).accept { actual ->
                compareBitmap("web_extensions/actions/button/expected.png", actual!!)
                svg.complete(null)
            }
        }

        sessionRule.waitForResult(svg)
    }

    @Test
    fun setIconPng() {
        val png100 = GeckoResult<Void>()
        val png38 = GeckoResult<Void>()
        val png19 = GeckoResult<Void>()
        val png10 = GeckoResult<Void>()

        testActionApi("""{
           "action": "setIcon",
           "path": {
             "19": "button/geo-19.png",
             "38": "button/geo-38.png"
           }
        }""") { action ->
            assertEquals(action.title, "Test action default")
            assertEquals(action.enabled, true)

            action.icon!!.get(100).accept { actual ->
                compareBitmap("web_extensions/actions/button/geo-38.png", actual!!)
                png100.complete(null)
            }

            action.icon!!.get(38).accept { actual ->
                compareBitmap("web_extensions/actions/button/geo-38.png", actual!!)
                png38.complete(null)
            }

            action.icon!!.get(19).accept { actual ->
                compareBitmap("web_extensions/actions/button/geo-19.png", actual!!)
                png19.complete(null)
            }

            action.icon!!.get(10).accept { actual ->
                compareBitmap("web_extensions/actions/button/geo-19.png", actual!!)
                png10.complete(null)
            }
        }

        sessionRule.waitForResult(png100)
        sessionRule.waitForResult(png38)
        sessionRule.waitForResult(png19)
        sessionRule.waitForResult(png10)
    }

    @Test
    fun setIconError() {
        val error = GeckoResult<Void>()

        testActionApi("""{
            "action": "setIcon",
            "path": "invalid/path/image.png"
        }""") { action ->
            action.icon!!.get(38).accept({
                error.completeExceptionally(RuntimeException("Should not succeed."))
            }, { exception ->
                assertTrue(exception is IllegalArgumentException)
                error.complete(null)
            })
        }

        sessionRule.waitForResult(error)
    }

    @Test
    @GeckoSessionTestRule.WithDisplay(width=100, height=100)
    @Ignore // this test fails intermittently on try :(
    fun testOpenPopup() {
        // First, let's make sure we have a popup set
        val actionResult = GeckoResult<Void>()
        testActionApi("""{
           "action": "setPopup",
           "popup": "test-popup.html"
        }""") { action ->
            assertEquals(action.title, "Test action default")
            assertEquals(action.enabled, true)

            actionResult.complete(null)
        }

        val url = when(id) {
            "#browserAction" -> "/test-open-popup-browser-action.html"
            "#pageAction" -> "/test-open-popup-page-action.html"
            else -> throw IllegalArgumentException()
        }

        windowPort!!.postMessage(JSONObject("""{
            "type": "load",
            "url": "$url"
        }"""))

        val openPopup = GeckoResult<Void>()
        sessionRule.session.setWebExtensionActionDelegate(extension!!,
                object : WebExtension.ActionDelegate {
            override fun onOpenPopup(extension: WebExtension,
                                     popupAction: WebExtension.Action): GeckoResult<GeckoSession>? {
                assertEquals(extension, this@ExtensionActionTest.extension)
                // assertEquals(popupAction, this@ExtensionActionTest.default)
                openPopup.complete(null)
                return null
            }
        })

        sessionRule.waitForPageStops(2)
        // openPopup needs user activation
        sessionRule.session.synthesizeTap(50, 50)

        sessionRule.waitForResult(openPopup)
    }

    @Test
    fun testClickWhenPopupIsNotDefined() {
        val pong = GeckoResult<Void>()

        backgroundPort!!.setDelegate(object : WebExtension.PortDelegate {
            override fun onPortMessage(message: Any, port: WebExtension.Port) {
                val json = message as JSONObject
                if (json.getString("method") == "pong") {
                    pong.complete(null)
                } else {
                    // We should NOT receive onClicked here
                    pong.completeExceptionally(IllegalArgumentException(
                            "Received unexpected: ${json.getString("method")}"))
                }
            }
        })

        val actionResult = GeckoResult<WebExtension.Action>()

        testActionApi("""{
           "action": "setPopup",
           "popup": "test-popup.html"
        }""") { action ->
            assertEquals(action.title, "Test action default")
            assertEquals(action.enabled, true)

            actionResult.complete(action)
        }

        val togglePopup = GeckoResult<Void>()
        val action = sessionRule.waitForResult(actionResult)

        extension!!.setActionDelegate(object : WebExtension.ActionDelegate {
            override fun onTogglePopup(extension: WebExtension,
                                     popupAction: WebExtension.Action): GeckoResult<GeckoSession>? {
                assertEquals(extension, this@ExtensionActionTest.extension)
                assertEquals(popupAction, action)
                togglePopup.complete(null)
                return null
            }
        })

        // This click() will not cause an onClicked callback because popup is set
        action.click()

        // but it will cause togglePopup to be called
        sessionRule.waitForResult(togglePopup)

        // If the response to ping reaches us before the onClicked we know onClicked wasn't called
        backgroundPort!!.postMessage(JSONObject("""{
            "type": "ping"
        }"""))

        sessionRule.waitForResult(pong)
    }

    @Test
    fun testClickWhenPopupIsDefined() {
        val onClicked = GeckoResult<Void>()
        backgroundPort!!.setDelegate(object : WebExtension.PortDelegate {
            override fun onPortMessage(message: Any, port: WebExtension.Port) {
                val json = message as JSONObject
                assertEquals(json.getString("method"), "onClicked")
                assertEquals(json.getString("type"), type)
                onClicked.complete(null)
            }
        })

        testActionApi("""{
           "action": "setPopup",
           "popup": null
        }""") { action ->
            assertEquals(action.title, "Test action default")
            assertEquals(action.enabled, true)

            // This click() WILL cause an onClicked callback
            action.click()
        }

        sessionRule.waitForResult(onClicked)
    }
}

