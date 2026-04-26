#!/usr/bin/env python3
"""Home Assistant MCP Server - exposes HA REST API as MCP tools."""

import asyncio
import json
import os
from typing import Any

import httpx
from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import (
    CallToolResult,
    ListToolsResult,
    TextContent,
    Tool,
)

HA_URL = os.environ.get("HA_URL", "http://homeassistant.local:8123")
HA_TOKEN = os.environ.get("HA_TOKEN", "")

app = Server("homeassistant")


def _headers() -> dict[str, str]:
    return {
        "Authorization": f"Bearer {HA_TOKEN}",
        "Content-Type": "application/json",
    }


async def _get(path: str) -> Any:
    async with httpx.AsyncClient() as client:
        r = await client.get(f"{HA_URL}/api{path}", headers=_headers(), timeout=10)
        r.raise_for_status()
        return r.json()


async def _post(path: str, payload: dict) -> Any:
    async with httpx.AsyncClient() as client:
        r = await client.post(
            f"{HA_URL}/api{path}", headers=_headers(), json=payload, timeout=10
        )
        r.raise_for_status()
        return r.json()


@app.list_tools()
async def list_tools() -> ListToolsResult:
    return ListToolsResult(
        tools=[
            Tool(
                name="get_state",
                description="Get the current state of a Home Assistant entity.",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "entity_id": {
                            "type": "string",
                            "description": "Entity ID, e.g. light.living_room",
                        }
                    },
                    "required": ["entity_id"],
                },
            ),
            Tool(
                name="get_states",
                description="Get states of all entities, optionally filtered by domain.",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "domain": {
                            "type": "string",
                            "description": "Optional domain filter, e.g. light, switch, sensor",
                        }
                    },
                },
            ),
            Tool(
                name="call_service",
                description=(
                    "Call a Home Assistant service. "
                    "Example: domain=light, service=turn_on, data={entity_id: light.kitchen}"
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "domain": {"type": "string", "description": "Service domain, e.g. light"},
                        "service": {"type": "string", "description": "Service name, e.g. turn_on"},
                        "data": {
                            "type": "object",
                            "description": "Service call data (entity_id, brightness, etc.)",
                        },
                    },
                    "required": ["domain", "service"],
                },
            ),
            Tool(
                name="get_services",
                description="List all available Home Assistant services.",
                inputSchema={"type": "object", "properties": {}},
            ),
            Tool(
                name="render_template",
                description="Render a Home Assistant Jinja2 template and return the result.",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "template": {
                            "type": "string",
                            "description": "Jinja2 template string, e.g. {{ states('sensor.temperature') }}",
                        }
                    },
                    "required": ["template"],
                },
            ),
            Tool(
                name="get_history",
                description="Get the state history of an entity for the last N hours.",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "entity_id": {"type": "string", "description": "Entity ID"},
                        "hours": {
                            "type": "integer",
                            "description": "How many hours of history to fetch (default 1)",
                            "default": 1,
                        },
                    },
                    "required": ["entity_id"],
                },
            ),
            Tool(
                name="get_config",
                description="Get the Home Assistant instance configuration.",
                inputSchema={"type": "object", "properties": {}},
            ),
            Tool(
                name="fire_event",
                description="Fire a custom Home Assistant event.",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "event_type": {"type": "string", "description": "Event type name"},
                        "event_data": {
                            "type": "object",
                            "description": "Optional event data payload",
                        },
                    },
                    "required": ["event_type"],
                },
            ),
        ]
    )


@app.call_tool()
async def call_tool(name: str, arguments: dict) -> CallToolResult:
    try:
        if name == "get_state":
            entity_id = arguments["entity_id"]
            result = await _get(f"/states/{entity_id}")
            return CallToolResult(content=[TextContent(type="text", text=json.dumps(result, indent=2))])

        elif name == "get_states":
            all_states = await _get("/states")
            domain = arguments.get("domain")
            if domain:
                all_states = [s for s in all_states if s["entity_id"].startswith(f"{domain}.")]
            return CallToolResult(content=[TextContent(type="text", text=json.dumps(all_states, indent=2))])

        elif name == "call_service":
            domain = arguments["domain"]
            service = arguments["service"]
            data = arguments.get("data", {})
            result = await _post(f"/services/{domain}/{service}", data)
            return CallToolResult(content=[TextContent(type="text", text=json.dumps(result, indent=2))])

        elif name == "get_services":
            result = await _get("/services")
            return CallToolResult(content=[TextContent(type="text", text=json.dumps(result, indent=2))])

        elif name == "render_template":
            result = await _post("/template", {"template": arguments["template"]})
            return CallToolResult(content=[TextContent(type="text", text=str(result))])

        elif name == "get_history":
            entity_id = arguments["entity_id"]
            hours = arguments.get("hours", 1)
            from datetime import datetime, timedelta, timezone

            start = (datetime.now(timezone.utc) - timedelta(hours=hours)).isoformat()
            result = await _get(
                f"/history/period/{start}?filter_entity_id={entity_id}&end_time="
                + datetime.now(timezone.utc).isoformat()
            )
            return CallToolResult(content=[TextContent(type="text", text=json.dumps(result, indent=2))])

        elif name == "get_config":
            result = await _get("/config")
            return CallToolResult(content=[TextContent(type="text", text=json.dumps(result, indent=2))])

        elif name == "fire_event":
            event_type = arguments["event_type"]
            event_data = arguments.get("event_data", {})
            result = await _post(f"/events/{event_type}", event_data)
            return CallToolResult(content=[TextContent(type="text", text=json.dumps(result, indent=2))])

        else:
            return CallToolResult(
                content=[TextContent(type="text", text=f"Unknown tool: {name}")],
                isError=True,
            )

    except httpx.HTTPStatusError as e:
        return CallToolResult(
            content=[TextContent(type="text", text=f"HTTP error {e.response.status_code}: {e.response.text}")],
            isError=True,
        )
    except Exception as e:
        return CallToolResult(
            content=[TextContent(type="text", text=f"Error: {e}")],
            isError=True,
        )


async def main():
    async with stdio_server() as (read_stream, write_stream):
        await app.run(read_stream, write_stream, app.create_initialization_options())


if __name__ == "__main__":
    asyncio.run(main())
